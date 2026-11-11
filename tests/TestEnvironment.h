#pragma once

// Launch-time test configuration must not race with a case changing the process
// environment. Linux retains the exec environment separately in this proc file:
// https://man7.org/linux/man-pages/man5/proc_pid_environ.5.html
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace test_support {
struct LaunchEnvironment {
    std::map<std::string, std::string, std::less<>> values;
    std::string error;
};
inline const LaunchEnvironment &launchEnvironment()
{
    static const auto environment = [] {
        LaunchEnvironment result;
        std::ifstream input("/proc/self/environ", std::ios::binary);
        if (!input) {
            result.error = "Cannot read the test launch environment";
            return result;
        }
        for (std::string entry; std::getline(input, entry, '\0');)
            if (const auto at = entry.find('='); at != std::string::npos)
                result.values.try_emplace(entry.substr(0, at), entry.substr(at + 1));
        if (input.bad()) result.error = "Could not finish reading the test launch environment";
        return result;
    }();
    return environment;
}
inline std::optional<std::string> launchEnvironment(std::string_view name)
{
    const auto &values = launchEnvironment().values;
    const auto found = values.find(name);
    return found == values.end() ? std::nullopt : std::optional(found->second);
}
}
