#pragma once

// The Git executable every Git fixture runs, the environment that keeps it away from the invoking
// user's and the system's configuration, and a way to run it without a shell. Qt-free, so the
// engine tests use it for `git merge-file` too.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace test_support {

// BRANCH_TRUCE_TEST_GIT when it is an absolute path, else the first `git` on PATH, else
// /usr/bin/git. Chosen once, before a case can change PATH.
inline const std::string &fixtureGit()
{
    static const std::string selected = [] {
        if (const char *chosen = std::getenv("BRANCH_TRUCE_TEST_GIT"); chosen && chosen[0] == '/')
            return std::string(chosen);
        const std::string_view path = std::getenv("PATH") ? std::getenv("PATH") : "";
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const auto end = std::min(path.find(':', begin), path.size());
            const auto directory = path.substr(begin, end - begin);
            if (directory.starts_with('/')) {
                const auto candidate = std::string(directory) + "/git";
                struct stat status{};
                if (::stat(candidate.c_str(), &status) == 0 && S_ISREG(status.st_mode)
                    && ::access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }
            begin = end + 1;
        }
        return std::string("/usr/bin/git");
    }();
    return selected;
}

// Name and value pairs for a fixture Git child: `home` stands in for HOME and the XDG config
// directory, `globalConfig` is the only global configuration it reads, the system configuration
// is off, nothing can prompt and messages stay untranslated.
inline std::vector<std::pair<std::string, std::string>> isolatedGitVariables(const std::string &home,
                                                                             const std::string &globalConfig)
{
    return {{"PATH", "/usr/bin:/bin"},
            {"LC_ALL", "C"},
            {"HOME", home},
            {"XDG_CONFIG_HOME", home + "/.config"},
            {"GIT_CONFIG_NOSYSTEM", "1"},
            {"GIT_CONFIG_GLOBAL", globalConfig},
            {"GIT_TERMINAL_PROMPT", "0"}};
}

// A process's standard output and its wait status, which is -1 when the
// process could not be started.
struct ProcessOutput {
    std::string text;
    int status = -1;
};

// Runs `arguments` without a shell and reads its standard output to the end. An absolute first
// argument runs that file; otherwise PATH is searched. `environment` replaces the inherited one.
inline ProcessOutput readProcess(std::vector<std::string> arguments,
                                 const std::optional<std::vector<std::string>> &environment = std::nullopt)
{
    ProcessOutput output;
    std::array<int, 2> ends{};
    if (pipe(ends.data()) != 0) return output;
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> variables = environment.value_or(std::vector<std::string>{});
    std::vector<char *> envp;
    for (auto &variable : variables) envp.push_back(variable.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    pid_t child = 0;
    char *const *childEnvironment = environment ? envp.data() : environ;
    const bool started = (arguments.front().starts_with('/')
                              ? posix_spawn(&child, argv.front(), &actions, nullptr, argv.data(), childEnvironment)
                              : posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), childEnvironment))
        == 0;
    posix_spawn_file_actions_destroy(&actions);
    close(ends[1]);
    std::array<char, 4096> buffer{};
    while (started) {
        const auto count = read(ends[0], buffer.data(), buffer.size());
        if (count == 0 || (count < 0 && errno != EINTR)) break;
        if (count > 0) output.text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    // Closing the read end first lets a child still writing end on SIGPIPE.
    close(ends[0]);
    if (!started) return output;
    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) return output;
    }
    output.status = status;
    return output;
}

} // namespace test_support
