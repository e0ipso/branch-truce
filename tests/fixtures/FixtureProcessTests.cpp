#include "TestHarness.h"
#include "fixtures/FixtureGit.h"

#include <filesystem>
#include <fstream>

using namespace test_support;
using namespace std::chrono_literals;

namespace {
// These modes deliberately exercise pipe ownership without a shell or a display.
int childMode(const std::string &mode)
{
    if (mode == "flood") {
        const std::string bytes(4096, 'x');
        while (true) {
            ::write(STDOUT_FILENO, bytes.data(), bytes.size());
            ::write(STDERR_FILENO, bytes.data(), bytes.size());
        }
    }
    if (mode == "ignored" || mode == "descendant" || mode == "closed-descendant") {
        std::array<int, 2> ready{};
        if (::pipe(ready.data()) != 0) return 98;
        if (mode == "ignored" || ::fork() == 0) {
            ::close(ready[0]);
            ::signal(SIGTERM, SIG_IGN);
            const auto pid = std::to_string(::getpid()) + "\n";
            ::write(STDOUT_FILENO, pid.data(), pid.size());
            if (mode == "closed-descendant") {
                ::close(STDOUT_FILENO);
                ::close(STDERR_FILENO);
            }
            if (mode != "ignored") ::write(ready[1], "r", 1);
            while (true) ::pause();
        }
        ::close(ready[1]);
        char byte;
        return ::read(ready[0], &byte, 1) == 1 ? 0 : 98;
    }
    if (mode == "echo") {
        if (std::getenv("HOME")) return 98;
        std::cout << std::filesystem::current_path().string() << '\n' << std::getenv("FIXTURE_VALUE") << '\n';
        std::cout << std::cin.rdbuf();
        std::cerr << "stderr";
        return 7;
    }
    return 99;
}

bool stopped(pid_t pid)
{
    // A killed orphan may remain a zombie until the host's init reaps it.
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    const auto end = line.rfind(')');
    return line.empty() || (end != std::string::npos && line.substr(end + 2, 1) == "Z");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--child") return childMode(argv[2]);
    configure(argc, argv);
    const auto executable = std::filesystem::canonical("/proc/self/exe").string();
    Suite suite("fixture-process");
    suite.run("empty commands and start failures have bounded diagnostics", [] {
        for (const auto &arguments : {std::vector<std::string>{}, {""}, {"/no/such/fixture-program"}}) {
            const auto result = readProcess(arguments);
            CHECK(result.status == -1 && !result.diagnostic.empty());
        }
        const auto badDirectory = readProcess({fixtureGit(), "--version"}, std::nullopt, "/no/such/fixture-directory");
        CHECK(badDirectory.status == -1 && !badDirectory.diagnostic.empty());
    });
    suite.run("stdout status stderr input environment and directory remain distinct", [&] {
        const std::string input(128 * 1024, 'i');
        const auto result = readProcess({executable, "--child", "echo"},
                                        std::vector<std::string>{"FIXTURE_VALUE=explicit"}, "/", input);
        CHECK(result.diagnostic.empty() && WIFEXITED(result.status) && WEXITSTATUS(result.status) == 7);
        CHECK(result.text == "/\nexplicit\n" + input);
        CHECK(result.error == "stderr");
        const auto git = readProcess({fixtureGit(), "--version"});
        CHECK(git.diagnostic.empty() && git.status == 0 && git.text.starts_with("git version "));
        const auto hash = readProcess({fixtureGit(), "hash-object", "--stdin"},
                                      std::vector<std::string>{"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null"},
                                      "/", "hello\n");
        CHECK(hash.diagnostic.empty() && hash.status == 0 && hash.text == "ce013625030ba8dba906f756967f9e9ca394464a\n");
    });
    suite.run("combined output flood fails within the retained byte budget", [&] {
        const auto result = readProcess({executable, "--child", "flood"}, std::nullopt, {}, {}, 2s, 8192);
        CHECK(result.status == -1 && result.diagnostic == "fixture combined output limit exceeded");
        CHECK(result.text.size() + result.error.size() == 8192);
    });
    suite.run("deadline kills ignored termination and reaps the leader", [&] {
        const auto before = std::chrono::steady_clock::now();
        const auto result = readProcess({executable, "--child", "ignored"}, std::nullopt, {}, {}, 200ms);
        CHECK(result.status == -1 && result.diagnostic == "fixture process deadline exceeded");
        CHECK(std::chrono::steady_clock::now() - before < 2s);
        CHECK(!result.text.empty());
        int status = 0;
        CHECK(::waitpid(std::stoi(result.text), &status, WNOHANG) == -1 && errno == ECHILD);
    });
    suite.run("exited leaders lose descendants with open or closed output without killing unrelated children", [&] {
        const auto unrelated = ::fork();
        CHECK(unrelated >= 0);
        if (unrelated == 0) {
            while (true) ::pause();
        }
        struct Reap {
            pid_t pid;
            ~Reap()
            {
                ::kill(pid, SIGKILL);
                while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
        } reap{unrelated};
        for (const auto *mode : {"descendant", "closed-descendant"}) {
            const auto result = readProcess({executable, "--child", mode}, std::nullopt, {}, {}, 2s);
            CHECK(result.diagnostic.empty() && result.status == 0 && !result.text.empty());
            const auto descendant = std::stoi(result.text);
            // No event-loop sleeps: poll proc until the kernel has delivered SIGKILL, bounded.
            const auto deadline = std::chrono::steady_clock::now() + 1s;
            while (!stopped(descendant) && std::chrono::steady_clock::now() < deadline) {}
            CHECK(stopped(descendant));
            CHECK(::kill(unrelated, 0) == 0 && !stopped(unrelated));
        }
    });
    return suite.report();
}
