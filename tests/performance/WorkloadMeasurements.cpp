// Generates representative files and adverse long lines, measures the product paths, and refuses
// to report a truncated or unfinished action as completed. Workload.h says where each part lives;
// this file holds the two profiles and what each one expects.
//
//   --profile gate      the small end of the matrix and the failure probes; the CTest entry.
//   --profile evidence  the full matrix, written to --report PATH and a .tsv beside it. PATH is
//                       required, since the run takes long and a relative default would land
//                       wherever it was started.
//
// Every size that matters to the product comes from core/ResourceLimits.h: the matrix measures
// each limit itself, and each over-limit probe sits one byte past its limit.
#include "core/ResourceLimits.h"
#include "performance/Workload.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using branch_truce::kLargeFileLines;
using branch_truce::kMaxGitReadBytes;
using branch_truce::kMaxLineBytes;
using branch_truce::kMaxRecoveryPayloadBytes;
using workload::Aggregate;
using workload::Spec;

std::vector<Spec> evidenceProfile()
{
    std::vector<Spec> specs;
    const auto add = [&](const char *group, const char *kind, std::string name, const char *mode, std::size_t lines,
                         std::size_t bytes, int timeoutMs, int repeats, bool escalating, bool startup) {
        specs.push_back({group, kind, std::move(name), mode, lines, bytes, timeoutMs, repeats, escalating, startup});
    };
    for (std::size_t lines : {std::size_t{200}, std::size_t{1000}, std::size_t{2000}, kLargeFileLines,
                              std::size_t{8000}, std::size_t{16000}})
        add("corr-lines", "correspondence", "correspondence-lines-" + std::to_string(lines), "lines", lines, 0,
            lines >= 8000 ? 45000 : 20000, 3, true, false);
    for (std::size_t bytes : {std::size_t{4096}, std::size_t{65536}, std::size_t{1048576}, kMaxRecoveryPayloadBytes})
        add("corr-long", "correspondence", "correspondence-long-" + std::to_string(bytes), "long", 0, bytes, 20000, 3,
            true, false);
    add("corr-control", "correspondence", "correspondence-identical-" + std::to_string(kLargeFileLines), "identical",
        kLargeFileLines, 0, 20000, 1, false, false);
    bool firstEditor = true;
    for (std::size_t lines :
         {std::size_t{200}, std::size_t{2000}, kLargeFileLines, std::size_t{10000}, std::size_t{20000}}) {
        add("editor-lines", "editor", "editor-lines-" + std::to_string(lines), "lines", lines, 0, 30000, 3, true,
            firstEditor);
        firstEditor = false;
    }
    for (std::size_t bytes : {std::size_t{1024}, std::size_t{8192}, std::size_t{65536}, kMaxLineBytes,
                              std::size_t{1048576}, std::size_t{4194304}})
        add("editor-long", "editor", "editor-long-" + std::to_string(bytes), "long", 0, bytes, 30000, 3, true, false);
    for (std::size_t bytes : {std::size_t{65536}, std::size_t{1048576}, kMaxGitReadBytes})
        add("git-read", "git", "git-read-" + std::to_string(bytes), "read", 0, bytes, 30000, 3, true, false);
    add("git-probe", "git", "git-read-over-limit", "read", 0, kMaxGitReadBytes + 1, 30000, 1, false, false);
    add("git-probe", "git", "git-input-over-limit", "input", 0, kMaxGitReadBytes + 1, 20000, 1, false, false);
    for (std::size_t bytes :
         {std::size_t{4096}, std::size_t{65536}, std::size_t{1048576}, std::size_t{4194304}, kMaxRecoveryPayloadBytes})
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
    add("git-probe", "git", "git-read-over-limit", "read", 0, kMaxGitReadBytes + 1, 30000, false);
    add("git-probe", "git", "git-input-over-limit", "input", 0, kMaxGitReadBytes + 1, 20000, false);
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

struct Tally {
    int tests = 0;
    int failures = 0;
    void pass(const std::string &name)
    {
        ++tests;
        std::cout << "PASS " << name << '\n';
    }
    void fail(const std::string &name, const std::string &why)
    {
        ++tests;
        ++failures;
        std::cerr << "FAIL " << name << ' ' << why << '\n';
    }
};

// The recorded evidence must support exactly the limits the product ships: a change to either
// side fails here until the evidence is measured again and the limit re-derived from it.
void expectRecordedEnvelope(Tally &tally)
{
#ifdef BRANCH_TRUCE_EVIDENCE_TSV
    std::string error;
    const auto rows = workload::readEvidenceRows(BRANCH_TRUCE_EVIDENCE_TSV, &error);
    if (rows.empty()) {
        tally.fail("recorded envelope", error);
        return;
    }
    for (const auto &limit : workload::compareWithLimits(workload::proposedEnvelope(rows))) {
        const auto name = "recorded envelope matches " + limit.name;
        if (limit.agrees())
            tally.pass(name);
        else
            tally.fail(name,
                       "limit " + std::to_string(limit.limit) + " recorded "
                           + (limit.measured ? std::to_string(*limit.measured) : std::string("none")));
    }
#else
    tally.fail("recorded envelope", "built without BRANCH_TRUCE_EVIDENCE_TSV");
#endif
}

void expect(Tally &tally, const std::vector<Aggregate> &results, const std::vector<Expectation> &expectations)
{
    for (const auto &expected : expectations) {
        const Aggregate *found = nullptr;
        for (const auto &item : results)
            if (item.name == expected.name) found = &item;
        if (found == nullptr || found->outcome != expected.outcome
            || (!expected.detail.empty() && found->detail.find(expected.detail) == std::string::npos)) {
            tally.fail(expected.name,
                       "expected " + expected.outcome + (expected.detail.empty() ? "" : " detail " + expected.detail)
                           + " actual " + (found == nullptr ? "missing" : found->outcome + " " + found->detail));
        } else {
            tally.pass(expected.name);
        }
    }
}

// Both profiles: an action over a product limit fails explicitly instead of passing truncated.
const std::vector<Expectation> &honestyExpectations()
{
    static const std::vector<Expectation> values = {
        {"git-read-over-limit", "explicit-failure", "api=OutputLimit"},
        {"git-read-over-limit", "explicit-failure", "incomplete=1"},
        {"git-input-over-limit", "explicit-failure", "api=Rejected"},
        {"recovery-bad-digest", "explicit-failure", "acknowledged=0"},
        {"recovery-overload", "explicit-failure", "acknowledged=0"},
    };
    return values;
}

const std::vector<Expectation> &gateExpectations()
{
    static const std::vector<Expectation> values = {
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
    return values;
}
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
    const bool evidence = profile == "evidence";
    if (evidence && report.empty()) {
        std::cerr << "the evidence profile needs --report PATH\n";
        return 2;
    }
    std::vector<std::string> problems;
    const auto results = workload::measure(evidence ? evidenceProfile() : gateProfile(), &problems);
    std::string cleanup = problems.empty() ? "scratch directories removed" : "";
    for (const auto &problem : problems) cleanup += (cleanup.empty() ? "" : "; ") + problem;
    if (evidence) {
        std::string error;
        if (!workload::writeReports(report, results, cleanup, &error)) {
            std::cerr << "report not written: " << error << '\n';
            return 1;
        }
        std::cout << "report " << report << '\n';
    }
    std::vector<Expectation> expectations = evidence ? honestyExpectations() : gateExpectations();
    if (!evidence) expectations.insert(expectations.end(), honestyExpectations().begin(), honestyExpectations().end());
    for (const auto &item : results) {
        if (item.outcome == "fabricated-success") expectations.push_back({item.name, "completed", ""});
    }
    Tally tally;
    expect(tally, results, expectations);
    if (!evidence) {
        expectRecordedEnvelope(tally);
    }
    std::cout << "RESULT workload-measurements tests=" << tally.tests << " assertions=" << tally.tests
              << " failures=" << tally.failures << '\n';
    return tally.failures == 0 ? 0 : 1;
}
