#pragma once

// Qt-free fixture children own a process group, a deadline and a combined stdout/stderr budget.
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace test_support {
struct ProcessOutput {
    std::string text;
    int status = -1; // waitpid status; -1 means supervision or startup failed.
    std::string error;
    std::string diagnostic;
};

inline ProcessOutput readProcess(std::vector<std::string> arguments,
                                 const std::optional<std::vector<std::string>> &environment = std::nullopt,
                                 const std::string &directory = {}, const std::string &input = {},
                                 std::chrono::milliseconds timeout = std::chrono::seconds(15),
                                 std::size_t outputLimit = 16 * 1024 * 1024)
{
    ProcessOutput output;
    if (arguments.empty() || arguments.front().empty()) {
        output.diagnostic = "empty fixture command";
        return output;
    }
    // Cleanup also runs if a string allocation throws. Keep the leader unreaped until the group
    // is killed, so its pid cannot be reused as an unrelated process group during cleanup.
    struct Child {
        pid_t pid = 0;
        std::array<int, 6> fd{-1, -1, -1, -1, -1, -1};
        ~Child()
        {
            if (pid > 0) {
                ::kill(-pid, SIGKILL);
                while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
            for (const int descriptor : fd)
                if (descriptor >= 0) ::close(descriptor);
        }
        void close(std::size_t index)
        {
            ::close(fd[index]);
            fd[index] = -1;
        }
    } child;
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, child.fd.data()) != 0
        || ::pipe2(child.fd.data() + 2, O_CLOEXEC) != 0 || ::pipe2(child.fd.data() + 4, O_CLOEXEC) != 0) {
        output.diagnostic = "fixture pipe creation failed: " + std::to_string(errno);
        return output;
    }
    for (auto &descriptor : child.fd) {
        if (descriptor > STDERR_FILENO) continue;
        const int replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) {
            output.diagnostic = "fixture descriptor duplication failed";
            return output;
        }
        ::close(descriptor);
        descriptor = replacement;
    }
    std::vector<char *> argv, envp;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    auto variables = environment.value_or(std::vector<std::string>{});
    for (auto &variable : variables) envp.push_back(variable.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int setup = posix_spawn_file_actions_init(&actions);
    if (setup) {
        output.diagnostic = "fixture spawn actions failed: " + std::to_string(setup);
        return output;
    }
    setup = posix_spawnattr_init(&attributes);
    if (setup) {
        posix_spawn_file_actions_destroy(&actions);
        output.diagnostic = "fixture spawn attributes failed: " + std::to_string(setup);
        return output;
    }
    const auto record = [&](int result) {
        if (!setup) setup = result;
    };
    record(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    record(posix_spawnattr_setpgroup(&attributes, 0));
    record(posix_spawn_file_actions_adddup2(&actions, child.fd[1], STDIN_FILENO));
    record(posix_spawn_file_actions_adddup2(&actions, child.fd[3], STDOUT_FILENO));
    record(posix_spawn_file_actions_adddup2(&actions, child.fd[5], STDERR_FILENO));
    for (int descriptor : child.fd) record(posix_spawn_file_actions_addclose(&actions, descriptor));
    if (!directory.empty()) record(posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!setup)
        setup = posix_spawnp(&child.pid, argv.front(), &actions, &attributes, argv.data(),
                             environment ? envp.data() : environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (setup) {
        child.pid = 0;
        output.diagnostic = "fixture start failed: " + std::to_string(setup);
        return output;
    }
    child.close(1);
    child.close(3);
    child.close(5);
    for (std::size_t index : {2u, 4u}) {
        if (::fcntl(child.fd[index], F_SETFL, O_NONBLOCK) < 0) {
            output.diagnostic = "fixture nonblocking pipe failed";
            return output;
        }
    }
    std::size_t sent = 0;
    bool exited = false;
    std::array<char, 4096> buffer{};
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            output.diagnostic = "fixture process deadline exceeded";
            return output;
        }
        if (child.fd[0] >= 0) {
            const auto count =
                ::send(child.fd[0], input.data() + sent, input.size() - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (count > 0) sent += static_cast<std::size_t>(count);
            if (sent == input.size() || (count < 0 && (errno == EPIPE || errno == ECONNRESET)))
                child.close(0);
            else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                output.diagnostic = "fixture input failed: " + std::to_string(errno);
                return output;
            }
        }
        // One read per stream per turn keeps floods from starving deadline and leader checks.
        for (std::size_t index : {2u, 4u}) {
            if (child.fd[index] < 0) continue;
            const auto count = ::read(child.fd[index], buffer.data(), buffer.size());
            if (count == 0)
                child.close(index);
            else if (count > 0) {
                const auto size = static_cast<std::size_t>(count);
                const auto retained = std::min(size, outputLimit - output.text.size() - output.error.size());
                (index == 2 ? output.text : output.error).append(buffer.data(), retained);
                if (retained < size) {
                    output.diagnostic = "fixture combined output limit exceeded";
                    return output;
                }
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                output.diagnostic = "fixture output read failed: " + std::to_string(errno);
                return output;
            }
        }
        if (!exited) {
            siginfo_t info{};
            if (::waitid(P_PID, static_cast<id_t>(child.pid), &info, WEXITED | WNOHANG | WNOWAIT) < 0
                && errno != EINTR) {
                output.diagnostic = "fixture child observation failed";
                return output;
            }
            if (info.si_pid == child.pid) {
                exited = true;
                // Descendants may retain the pipes after a successful leader exit.
                ::kill(-child.pid, SIGKILL);
            }
        }
        if (exited && child.fd[2] < 0 && child.fd[4] < 0) {
            if (::waitpid(child.pid, &output.status, 0) == child.pid)
                child.pid = 0;
            else if (errno == EINTR)
                continue;
            else
                output.diagnostic = "fixture child reap failed";
            return output;
        }
        std::array<pollfd, 3> polls{{{child.fd[0], POLLOUT, 0}, {child.fd[2], POLLIN, 0}, {child.fd[4], POLLIN, 0}}};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (::poll(polls.data(), polls.size(), static_cast<int>(std::clamp<long long>(remaining, 0, 10))) < 0
            && errno != EINTR) {
            output.diagnostic = "fixture poll failed";
            return output;
        }
    }
}
} // namespace test_support
