#pragma once

// The Git executable every Git fixture runs, the environment that keeps it away from the invoking
// user's and the system's configuration, and a way to run it without a shell. Qt-free, so the
// engine tests use it for `git merge-file` too.

#include "fixtures/FixtureProcess.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/stat.h>
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

} // namespace test_support
