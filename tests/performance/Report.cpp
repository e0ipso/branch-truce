// The evidence report: the host and build the run measured, every aggregate, the proposed
// envelope under the decision rule, and the outcomes that stay outside it.
#include "performance/Workload.h"

#include <QtGlobal>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BRANCH_TRUCE_MEASUREMENT_BUILD
#define BRANCH_TRUCE_MEASUREMENT_BUILD "unspecified"
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
        if (capGit && item.bytes > kGitGuard) continue;
        if (!best || size > *best) best = size;
    }
    return best;
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
} // namespace

void writeReports(const std::string &path, const std::vector<Aggregate> &results, const std::string &cleanup)
{
    std::ofstream markdown(path);
    utsname system{};
    uname(&system);
    const auto mount = capture({"/usr/bin/findmnt", "-T", "/var/tmp", "-n", "-o", "FSTYPE,SOURCE"});
    markdown << "# Workload measurements\n\n";
    markdown << "Build type " << BRANCH_TRUCE_MEASUREMENT_BUILD << ". Compiler " << __VERSION__ << ". Qt " << qVersion()
             << ". Git " << capture({"/usr/bin/git", "--version"}) << ".\n\n";
    markdown << "Machine " << system.sysname << " " << system.release << " " << system.machine << ", " << cpuModel()
             << ", " << sysconf(_SC_NPROCESSORS_ONLN) << " processors, " << memTotal() << ". Scratch filesystem "
             << (mount.empty() ? "not reported" : mount) << ".\n\n";
    markdown << "Editor samples use QT_QPA_PLATFORM=offscreen and QT_QUICK_BACKEND=software. "
             << "They are not a Wayland or KDE session. Recovery and Git scratch live under /var/tmp "
             << "because /tmp is tmpfs on this host.\n\n";
    markdown << "## Decision rule\n\n";
    markdown
        << "A completed sample can enter the proposed envelope only when every repeat completed "
        << "and the median elapsed time is within the budget. The proposed size is the largest measured "
        << "point that qualifies. Sizes that were not attempted are not claimed. Timeouts, resource kills, "
        << "truncation, unacknowledged edits, explicit failures and fabricated success stay outside the envelope.\n\n";
    markdown << "Budgets are " << budgetText() << ". The Git figure cannot exceed the existing 8 MiB output guard.\n\n";
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
    const auto corrLines = qualifying(results, "corr-lines", "construct", SizeKind::Lines, false);
    const auto corrEdit = qualifying(results, "corr-lines", "edit", SizeKind::Lines, false);
    const auto editorLines = qualifying(results, "editor-lines", "load", SizeKind::Lines, false);
    const auto editorKey = qualifying(results, "editor-lines", "key", SizeKind::Lines, false);
    const auto longLoad = qualifying(results, "editor-long", "load", SizeKind::MaxLine, false);
    const auto longKey = qualifying(results, "editor-long", "key", SizeKind::MaxLine, false);
    const auto longConstruct = qualifying(results, "corr-long", "construct", SizeKind::MaxLine, false);
    const auto longEdit = qualifying(results, "corr-long", "edit", SizeKind::MaxLine, false);
    const auto gitRead = qualifying(results, "git-read", "read", SizeKind::Bytes, true);
    const auto recoveryCommit = qualifying(results, "recovery", "commit", SizeKind::Bytes, false);
    const auto recoveryReload = qualifying(results, "recovery", "reload", SizeKind::Bytes, false);
    const auto smallest = [](std::initializer_list<std::optional<std::uint64_t>> values) {
        std::optional<std::uint64_t> best;
        for (const auto &value : values) {
            if (!value) return std::optional<std::uint64_t>{};
            if (!best || *value < *best) best = value;
        }
        return best;
    };
    markdown << "\n## Proposed resource envelope\n\n";
    markdown << "Component bounds before taking the tightest measured point: correspondence construction "
             << sizeOrNone(corrLines, "lines") << ", correspondence edit " << sizeOrNone(corrEdit, "lines")
             << ", editor load " << sizeOrNone(editorLines, "lines") << ", editor keystroke "
             << sizeOrNone(editorKey, "lines") << ", long-line editor load " << sizeOrNone(longLoad, "bytes")
             << ", long-line editor keystroke " << sizeOrNone(longKey, "bytes")
             << ", long-line correspondence construction " << sizeOrNone(longConstruct, "bytes")
             << ", long-line correspondence edit " << sizeOrNone(longEdit, "bytes") << ", Git read "
             << sizeOrNone(gitRead, "bytes") << ", recovery commit " << sizeOrNone(recoveryCommit, "bytes")
             << ", recovery reload " << sizeOrNone(recoveryReload, "bytes") << ".\n\n";
    markdown << "Representative files, three sources, width 96 bytes including the newline: up to "
             << sizeOrNone(smallest({corrLines, corrEdit, editorLines, editorKey}), "lines") << ".\n\n";
    markdown << "Adverse long lines, three panes or three sources: up to "
             << sizeOrNone(smallest({longLoad, longKey, longConstruct, longEdit}), "bytes on the longest line")
             << ".\n\n";
    markdown << "Git process output: up to " << sizeOrNone(gitRead, "bytes")
             << ", and not above the existing 8 MiB guard. The guard was not changed. ";
    for (const auto &item : results) {
        if (item.name == "git-read-8388608" && item.outcome != "completed")
            markdown << "The 8388608-byte blob did not complete: " << item.outcome << ". " << item.detail << ". ";
    }
    markdown << "\n\n";
    markdown << "Recovery acknowledgment, commit through fsync plus a following reload: up to "
             << sizeOrNone(smallest({recoveryCommit, recoveryReload}), "payload bytes") << ".\n\n";
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
