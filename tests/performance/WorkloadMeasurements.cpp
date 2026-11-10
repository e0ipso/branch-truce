// Generates representative files and adverse long lines, measures the product paths, and refuses
// to report a truncated or unfinished action as completed. Workload.h says where each part lives;
// this file holds the two profiles and what each one expects.
//
//   --profile gate      the small end of the matrix and the failure probes; the CTest entry.
//   --profile evidence  the full matrix, written to --report and a .tsv beside it.
#include "performance/Workload.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
using workload::Aggregate;
using workload::kGitGuard;
using workload::Spec;

std::vector<Spec> evidenceProfile()
{
    std::vector<Spec> specs;
    const auto add = [&](const char *group, const char *kind, std::string name, const char *mode, std::size_t lines,
                         std::size_t bytes, int timeoutMs, int repeats, bool escalating, bool startup) {
        specs.push_back({group, kind, std::move(name), mode, lines, bytes, timeoutMs, repeats, escalating, startup});
    };
    for (std::size_t lines : {std::size_t{200}, std::size_t{1000}, std::size_t{2000}, std::size_t{4000},
                              std::size_t{8000}, std::size_t{16000}})
        add("corr-lines", "correspondence", "correspondence-lines-" + std::to_string(lines), "lines", lines, 0,
            lines >= 8000 ? 45000 : 20000, 3, true, false);
    for (std::size_t bytes : {std::size_t{4096}, std::size_t{65536}, std::size_t{1048576}, kGitGuard})
        add("corr-long", "correspondence", "correspondence-long-" + std::to_string(bytes), "long", 0, bytes, 20000, 3,
            true, false);
    add("corr-control", "correspondence", "correspondence-identical-4000", "identical", 4000, 0, 20000, 1, false,
        false);
    bool firstEditor = true;
    for (std::size_t lines : {std::size_t{200}, std::size_t{2000}, std::size_t{10000}, std::size_t{20000}}) {
        add("editor-lines", "editor", "editor-lines-" + std::to_string(lines), "lines", lines, 0, 30000, 3, true,
            firstEditor);
        firstEditor = false;
    }
    for (std::size_t bytes : {std::size_t{1024}, std::size_t{8192}, std::size_t{65536}, std::size_t{262144},
                              std::size_t{1048576}, std::size_t{4194304}})
        add("editor-long", "editor", "editor-long-" + std::to_string(bytes), "long", 0, bytes, 30000, 3, true, false);
    for (std::size_t bytes : {std::size_t{65536}, std::size_t{1048576}, std::size_t{4194304}, kGitGuard})
        add("git-read", "git", "git-read-" + std::to_string(bytes), "read", 0, bytes, 30000, 3, true, false);
    add("git-probe", "git", "git-read-over-limit", "read", 0, kGitGuard + 4096, 30000, 1, false, false);
    add("git-probe", "git", "git-input-over-limit", "input", 0, kGitGuard + 1, 20000, 1, false, false);
    for (std::size_t bytes :
         {std::size_t{4096}, std::size_t{65536}, std::size_t{1048576}, std::size_t{4194304}, kGitGuard})
        add("recovery", "recovery", "recovery-" + std::to_string(bytes), "commit", 0, bytes, 30000, 3, true, false);
    add("recovery-probe", "recovery", "recovery-bad-digest", "bad-digest", 0, 64, 20000, 1, false, false);
    add("recovery-probe", "recovery", "recovery-overload", "overload", 0, 1024ull * 1024ull, 20000, 1, false, false);
    return specs;
}

std::vector<Spec> gateProfile()
{
    std::vector<Spec> specs;
    const auto add = [&](const char *group, const char *kind, std::string name, const char *mode, std::size_t lines,
                         std::size_t bytes, int timeoutMs, bool startup) {
        specs.push_back({group, kind, std::move(name), mode, lines, bytes, timeoutMs, 1, false, startup});
    };
    add("corr-lines", "correspondence", "correspondence-lines-200", "lines", 200, 0, 20000, false);
    add("corr-long", "correspondence", "correspondence-long-65536", "long", 0, 65536, 20000, false);
    add("editor-lines", "editor", "editor-lines-200", "lines", 200, 0, 20000, true);
    add("editor-long", "editor", "editor-long-8192", "long", 0, 8192, 20000, false);
    add("git-read", "git", "git-read-65536", "read", 0, 65536, 20000, false);
    add("git-probe", "git", "git-read-over-limit", "read", 0, kGitGuard + 4096, 30000, false);
    add("git-probe", "git", "git-input-over-limit", "input", 0, kGitGuard + 1, 20000, false);
    add("recovery", "recovery", "recovery-65536", "commit", 0, 65536, 20000, false);
    add("recovery-probe", "recovery", "recovery-bad-digest", "bad-digest", 0, 64, 20000, false);
    add("recovery-probe", "recovery", "recovery-overload", "overload", 0, 1024ull * 1024ull, 20000, false);
    add("self-check", "self-check", "worker-signalled-after-report", "signal", 0, 0, 20000, false);
    return specs;
}

struct Expectation {
    std::string name;
    std::string outcome;
    std::string detail;
};

int expect(const std::vector<Aggregate> &results, const std::vector<Expectation> &expectations)
{
    int tests = 0;
    int assertions = 0;
    int failures = 0;
    for (const auto &expected : expectations) {
        ++tests;
        ++assertions;
        const Aggregate *found = nullptr;
        for (const auto &item : results)
            if (item.name == expected.name) found = &item;
        if (found == nullptr || found->outcome != expected.outcome
            || (!expected.detail.empty() && found->detail.find(expected.detail) == std::string::npos)) {
            ++failures;
            std::cerr << "FAIL " << expected.name << " expected " << expected.outcome;
            if (!expected.detail.empty()) std::cerr << " detail " << expected.detail;
            std::cerr << " actual " << (found == nullptr ? "missing" : found->outcome + " " + found->detail) << '\n';
        } else {
            std::cout << "PASS " << expected.name << '\n';
        }
    }
    std::cout << "RESULT workload-measurements tests=" << tests << " assertions=" << assertions
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

// Both profiles: an action over a product limit fails explicitly instead of passing truncated.
const std::vector<Expectation> kHonesty = {
    {"git-read-over-limit", "explicit-failure", "api=OutputLimit"},
    {"git-read-over-limit", "explicit-failure", "incomplete=1"},
    {"git-input-over-limit", "explicit-failure", "api=Rejected"},
    {"recovery-bad-digest", "explicit-failure", "acknowledged=0"},
    {"recovery-overload", "explicit-failure", "acknowledged=0"},
};

const std::vector<Expectation> kGate = {
    {"correspondence-lines-200-construct", "completed", ""},
    {"correspondence-lines-200-edit", "completed", ""},
    {"correspondence-long-65536-construct", "completed", ""},
    {"correspondence-long-65536-edit", "completed", ""},
    {"editor-startup", "completed", ""},
    {"editor-lines-200-load", "completed", ""},
    {"editor-lines-200-key", "completed", ""},
    {"editor-long-8192-load", "completed", ""},
    {"editor-long-8192-key", "completed", ""},
    {"git-read-65536", "completed", "api=Success"},
    {"recovery-65536-commit", "completed", "acknowledged=1"},
    {"recovery-65536-reload", "completed", ""},
    // The supervisor itself: a record from a worker that then died on a signal is not completed.
    {"worker-signalled-after-report", "inconsistent", "worker ended by signal 15"},
};
} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--worker") return workload::workerMain(argc, argv);
    std::string profile = "gate";
    std::string report;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--profile" && i + 1 < argc)
            profile = argv[++i];
        else if (key == "--report" && i + 1 < argc)
            report = argv[++i];
        else if (key == "--help") {
            std::cout << "usage: workload-measurements --profile gate|evidence [--report PATH]\n";
            return 0;
        }
    }
    std::string problem;
    const bool evidence = profile == "evidence";
    const auto results = workload::measure(evidence ? evidenceProfile() : gateProfile(), &problem);
    const std::string cleanup = problem.empty() ? "scratch directories removed" : problem;
    if (evidence) {
        if (report.empty()) report = "tests/fixtures/performance/evidence.md";
        workload::writeReports(report, results, cleanup);
        std::cout << "report " << report << '\n';
    }
    std::vector<Expectation> expectations = evidence ? kHonesty : kGate;
    if (!evidence) expectations.insert(expectations.end(), kHonesty.begin(), kHonesty.end());
    for (const auto &item : results) {
        if (item.outcome == "fabricated-success") expectations.push_back({item.name, "completed", ""});
    }
    return expect(results, expectations);
}
