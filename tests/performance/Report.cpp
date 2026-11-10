// The evidence report: the host and build the run measured, every aggregate, the proposed
// envelope under the decision rule, and the outcomes that stay outside it.
#include "core/ResourceLimits.h"
#include "fixtures/FixtureGit.h"
#include "performance/Workload.h"

#include <QtGlobal>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BRANCH_TRUCE_MEASUREMENT_BUILD
#define BRANCH_TRUCE_MEASUREMENT_BUILD "unspecified"
#endif
#ifndef BRANCH_TRUCE_SOURCE_DIR
#define BRANCH_TRUCE_SOURCE_DIR "."
#endif

namespace workload {
namespace {

std::string capture(const std::vector<std::string> &args)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        std::vector<char *> pointers;
        for (const auto &arg : args) pointers.push_back(const_cast<char *>(arg.c_str()));
        pointers.push_back(nullptr);
        execvp(pointers[0], pointers.data());
        _exit(127);
    }
    ::close(pipefd[1]);
    std::string output;
    char buffer[256];
    ssize_t count = 0;
    while ((count = ::read(pipefd[0], buffer, sizeof buffer)) > 0)
        output.append(buffer, static_cast<std::size_t>(count));
    ::close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

std::string cpuModel()
{
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0) {
            const auto pos = line.find(':');
            if (pos != std::string::npos) {
                auto model = line.substr(pos + 1);
                while (!model.empty() && model.front() == ' ') model.erase(model.begin());
                return model;
            }
        }
    }
    return "not reported";
}

std::string memTotal()
{
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) return line;
    }
    return "MemTotal: not reported";
}

enum class SizeKind { Lines, MaxLine, Bytes };

// The largest size in `group` whose `metric` completed every repeat within its budget.
std::optional<std::uint64_t> qualifying(const std::vector<Aggregate> &results, const char *group, const char *metric,
                                        SizeKind kind, bool capGit)
{
    const auto *budget = findMetric(metric);
    std::optional<std::uint64_t> best;
    for (const auto &item : results) {
        if (item.group != group || item.metric != metric || item.outcome != "completed") continue;
        const auto mid = medianOf(item.elapsed);
        if (!mid || !budget || *mid > static_cast<std::uint64_t>(budget->budgetMs)) continue;
        const std::uint64_t size = kind == SizeKind::Lines ? item.lines
            : kind == SizeKind::MaxLine                    ? item.maxLine
                                                           : item.bytes;
        if (capGit && item.bytes > branch_truce::kMaxGitReadBytes) continue;
        if (!best || size > *best) best = size;
    }
    return best;
}

// The largest qualifying size of every component the envelope takes its tightest point from.
struct Components {
    std::optional<std::uint64_t> corrLines, corrEdit, editorLines, editorKey;
    std::optional<std::uint64_t> longLoad, longKey, longConstruct, longEdit;
    std::optional<std::uint64_t> gitRead, recoveryCommit, recoveryReload;
};

Components components(const std::vector<Aggregate> &results)
{
    Components bounds;
    bounds.corrLines = qualifying(results, "corr-lines", "construct", SizeKind::Lines, false);
    bounds.corrEdit = qualifying(results, "corr-lines", "edit", SizeKind::Lines, false);
    bounds.editorLines = qualifying(results, "editor-lines", "load", SizeKind::Lines, false);
    bounds.editorKey = qualifying(results, "editor-lines", "key", SizeKind::Lines, false);
    bounds.longLoad = qualifying(results, "editor-long", "load", SizeKind::MaxLine, false);
    bounds.longKey = qualifying(results, "editor-long", "key", SizeKind::MaxLine, false);
    bounds.longConstruct = qualifying(results, "corr-long", "construct", SizeKind::MaxLine, false);
    bounds.longEdit = qualifying(results, "corr-long", "edit", SizeKind::MaxLine, false);
    bounds.gitRead = qualifying(results, "git-read", "read", SizeKind::Bytes, true);
    bounds.recoveryCommit = qualifying(results, "recovery", "commit", SizeKind::Bytes, false);
    bounds.recoveryReload = qualifying(results, "recovery", "reload", SizeKind::Bytes, false);
    return bounds;
}

// The tightest of the values, or nothing when any of them is missing.
std::optional<std::uint64_t> smallest(std::initializer_list<std::optional<std::uint64_t>> values)
{
    std::optional<std::uint64_t> best;
    for (const auto &value : values) {
        if (!value) return std::nullopt;
        if (!best || *value < *best) best = value;
    }
    return best;
}

Envelope envelopeOf(const Components &bounds)
{
    return {smallest({bounds.corrLines, bounds.corrEdit, bounds.editorLines, bounds.editorKey}),
            smallest({bounds.longLoad, bounds.longKey, bounds.longConstruct, bounds.longEdit}), bounds.gitRead,
            smallest({bounds.recoveryCommit, bounds.recoveryReload})};
}

std::string sizeOrNone(const std::optional<std::uint64_t> &value, const char *unit)
{
    if (!value) return std::string("none of the measured sizes met the budget");
    return std::to_string(*value) + " " + unit;
}

std::string budgetText()
{
    std::string text;
    const auto count = std::size(kMetrics);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &metric = kMetrics[i];
        text += (i == 0               ? ""
                     : i + 1 == count ? ", and "
                                      : ", ")
            + std::string(metric.description) + ' ' + std::to_string(metric.budgetMs) + " ms";
    }
    return text;
}
// The commit the measured binary was built from, marked when the checkout had other changes.
std::string sourceRevision()
{
    const auto &git = test_support::fixtureGit();
    const auto revision = capture({git, "-C", BRANCH_TRUCE_SOURCE_DIR, "rev-parse", "HEAD"});
    if (revision.empty()) return "not reported";
    const auto changes = capture({git, "-C", BRANCH_TRUCE_SOURCE_DIR, "status", "--porcelain"});
    return revision + (changes.empty() ? "" : " with uncommitted changes");
}

std::optional<std::uint64_t> number(const std::string &field)
{
    if (field == "-" || field.empty()) return std::nullopt;
    char *end = nullptr;
    const auto value = std::strtoull(field.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return std::nullopt;
    return value;
}
} // namespace

Envelope proposedEnvelope(const std::vector<Aggregate> &results) { return envelopeOf(components(results)); }

std::vector<LimitComparison> compareWithLimits(const Envelope &envelope)
{
    return {{"kLargeFileLines", branch_truce::kLargeFileLines, envelope.lines},
            {"kMaxLineBytes", branch_truce::kMaxLineBytes, envelope.lineBytes},
            {"kMaxGitReadBytes", branch_truce::kMaxGitReadBytes, envelope.gitReadBytes},
            {"kMaxRecoveryPayloadBytes", branch_truce::kMaxRecoveryPayloadBytes, envelope.recoveryBytes}};
}

std::vector<Aggregate> readEvidenceRows(const std::string &path, std::string *error)
{
    std::ifstream table(path);
    if (!table) {
        *error = "cannot read " + path;
        return {};
    }
    std::vector<Aggregate> rows;
    std::string line;
    std::getline(table, line); // The header.
    while (std::getline(table, line)) {
        std::vector<std::string> fields;
        std::istringstream split(line);
        for (std::string field; std::getline(split, field, '\t');) fields.push_back(field);
        const auto bytes = fields.size() >= 13 ? number(fields[5]) : std::nullopt;
        const auto lines = fields.size() >= 13 ? number(fields[6]) : std::nullopt;
        const auto maxLine = fields.size() >= 13 ? number(fields[7]) : std::nullopt;
        if (!bytes || !lines || !maxLine) {
            *error = path + ": malformed row: " + line;
            return {};
        }
        Aggregate row;
        row.kind = fields[0];
        row.name = fields[1];
        row.metric = fields[2];
        row.group = fields[3];
        row.outcome = fields[4];
        row.bytes = *bytes;
        row.lines = *lines;
        row.maxLine = *maxLine;
        if (const auto median = number(fields[10])) row.elapsed.push_back(*median);
        rows.push_back(row);
    }
    if (table.bad() || rows.empty()) {
        *error = path + " has no rows";
        return {};
    }
    return rows;
}

void writeReports(const std::string &path, const std::vector<Aggregate> &results, const std::string &cleanup)
{
    std::ofstream markdown(path);
    utsname system{};
    uname(&system);
    const auto mount = capture({"findmnt", "-T", scratchBase(), "-n", "-o", "FSTYPE,SOURCE"});
    markdown << "# Workload measurements\n\n";
    markdown << "Source revision " << sourceRevision() << ". Build type " << BRANCH_TRUCE_MEASUREMENT_BUILD
             << ". Compiler " << __VERSION__ << ". Qt " << qVersion() << ". Git "
             << capture({test_support::fixtureGit(), "--version"}) << ".\n\n";
    markdown << "Machine " << system.sysname << " " << system.release << " " << system.machine << ", " << cpuModel()
             << ", " << sysconf(_SC_NPROCESSORS_ONLN) << " processors, " << memTotal() << ". Scratch filesystem "
             << (mount.empty() ? "not reported" : mount) << ".\n\n";
    markdown << "Editor samples use QT_QPA_PLATFORM=offscreen and QT_QUICK_BACKEND=software. "
             << "They are not a Wayland or KDE session. Recovery and Git scratch live under " << scratchBase()
             << ", on the scratch filesystem named above.\n\n";
    markdown << "## Decision rule\n\n";
    markdown
        << "A completed sample can enter the proposed envelope only when every repeat completed "
        << "and the median elapsed time is within the budget. The proposed size is the largest measured "
        << "point that qualifies. Sizes that were not attempted are not claimed. Timeouts, resource kills, "
        << "truncation, unacknowledged edits, explicit failures and fabricated success stay outside the envelope.\n\n";
    markdown << "Budgets are " << budgetText() << ". The Git figure cannot exceed the product's Git output limit, "
             << branch_truce::kMaxGitReadBytes << " bytes.\n\n";
    markdown << "## Results\n\n";
    markdown << "| Name | Outcome | Bytes | Lines | Max line | Samples | Min ms | Median ms | Max ms | RSS KiB |\n";
    markdown << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
    std::string tsvPath = path;
    const auto dot = tsvPath.rfind('.');
    if (dot == std::string::npos)
        tsvPath += ".tsv";
    else
        tsvPath.replace(dot, std::string::npos, ".tsv");
    std::ofstream table(tsvPath);
    table << "kind\tname\tmetric\tgroup\toutcome\tbytes\tlines\tmax_line\tsamples\tmin_ms\tmedian_ms\tmax_ms\trss_"
             "kb\tdetail\n";
    for (const auto &item : results) {
        const auto mid = medianOf(item.elapsed);
        const auto min = item.elapsed.empty()
            ? std::optional<std::uint64_t>{}
            : std::optional<std::uint64_t>{*std::min_element(item.elapsed.begin(), item.elapsed.end())};
        const auto max = item.elapsed.empty()
            ? std::optional<std::uint64_t>{}
            : std::optional<std::uint64_t>{*std::max_element(item.elapsed.begin(), item.elapsed.end())};
        const auto show = [](const std::optional<std::uint64_t> &value) {
            return value ? std::to_string(*value) : std::string("-");
        };
        markdown << "| " << item.name << " | " << item.outcome << " | " << item.bytes << " | " << item.lines << " | "
                 << item.maxLine << " | " << item.samples << " | " << show(min) << " | " << show(mid) << " | "
                 << show(max) << " | " << (item.rssKb == 0 ? std::string("-") : std::to_string(item.rssKb)) << " |\n";
        table << item.kind << '\t' << item.name << '\t' << item.metric << '\t' << item.group << '\t' << item.outcome
              << '\t' << item.bytes << '\t' << item.lines << '\t' << item.maxLine << '\t' << item.samples << '\t'
              << show(min) << '\t' << show(mid) << '\t' << show(max) << '\t' << item.rssKb << '\t'
              << sanitize(item.detail) << '\n';
    }
    const auto bounds = components(results);
    const auto envelope = envelopeOf(bounds);
    markdown << "\n## Proposed resource envelope\n\n";
    markdown << "Component bounds before taking the tightest measured point: correspondence construction "
             << sizeOrNone(bounds.corrLines, "lines") << ", correspondence edit "
             << sizeOrNone(bounds.corrEdit, "lines") << ", editor load " << sizeOrNone(bounds.editorLines, "lines")
             << ", editor keystroke " << sizeOrNone(bounds.editorKey, "lines") << ", long-line editor load "
             << sizeOrNone(bounds.longLoad, "bytes") << ", long-line editor keystroke "
             << sizeOrNone(bounds.longKey, "bytes") << ", long-line correspondence construction "
             << sizeOrNone(bounds.longConstruct, "bytes") << ", long-line correspondence edit "
             << sizeOrNone(bounds.longEdit, "bytes") << ", Git read " << sizeOrNone(bounds.gitRead, "bytes")
             << ", recovery commit " << sizeOrNone(bounds.recoveryCommit, "bytes") << ", recovery reload "
             << sizeOrNone(bounds.recoveryReload, "bytes") << ".\n\n";
    markdown << "Representative files, three sources, width 96 bytes including the newline: up to "
             << sizeOrNone(envelope.lines, "lines") << ".\n\n";
    markdown << "Adverse long lines, three panes or three sources: up to "
             << sizeOrNone(envelope.lineBytes, "bytes on the longest line") << ".\n\n";
    markdown << "Git process output: up to " << sizeOrNone(envelope.gitReadBytes, "bytes")
             << ", and not above the product's output limit. ";
    for (const auto &item : results) {
        if (item.group == "git-read" && item.outcome != "completed")
            markdown << "The " << item.bytes << "-byte blob did not complete: " << item.outcome << ". " << item.detail
                     << ". ";
    }
    markdown << "\n\n";
    markdown << "Recovery acknowledgment, commit through fsync plus a following reload: up to "
             << sizeOrNone(envelope.recoveryBytes, "payload bytes") << ".\n\n";
    markdown << "Product limits in src/core/ResourceLimits.h against this envelope:\n\n";
    for (const auto &limit : compareWithLimits(envelope))
        markdown << "- " << limit.name << " " << limit.limit << ", measured "
                 << (limit.measured ? std::to_string(*limit.measured) : std::string("none")) << ": "
                 << (limit.agrees() ? "agrees" : "differs") << "\n";
    markdown << "\n";
    markdown << "Completed samples over the latency budget stay outside the envelope:\n\n";
    for (const auto &item : results) {
        const auto mid = medianOf(item.elapsed);
        const auto *metric = findMetric(item.metric);
        if (item.outcome != "completed" || !mid || !metric) continue;
        if (*mid > static_cast<std::uint64_t>(metric->budgetMs))
            markdown << "- " << item.name << " median " << *mid << " ms, budget " << metric->budgetMs << " ms\n";
    }
    markdown << "\n";
    markdown << "Identical-buffer correspondence is a direct path and is excluded from the line-count proposal. "
             << "One failed or skipped probe means that part of the envelope is not established.\n\n";
    markdown << "## Outcomes outside the envelope\n\n";
    for (const auto &item : results) {
        if (item.outcome != "completed")
            markdown << "- " << item.name << " " << item.outcome << ". " << item.detail << "\n";
    }
    markdown << "\n## Limitations\n\n";
    markdown << "Each evidence size was repeated three times on one host. The median is not a population statistic. "
             << "Repeats can hit the page cache. The default developer build is Debug. These numbers come from the "
             << BRANCH_TRUCE_MEASUREMENT_BUILD << " binary. Editor latency does not include a physical keyboard, "
             << "input method, or KDE compositor. Correspondence cost is quadratic in line count. A long line is "
             << "interned once and is not the quadratic case. RSS is the process high-water mark and later work in "
             << "the same process can only raise it. Cleanup: " << cleanup << "\n";
}

} // namespace workload
