#pragma once

// What the workload measurements share: the metrics and their budgets, the workload kinds, the
// records a worker reports and the line format that carries them to the supervisor.
//
//   Workload.cpp    the record names, the line format and the helpers below.
//   Workers.cpp     generates each workload in a worker process and reports its records.
//   Supervisor.cpp  runs the workers, polices their memory and time, and aggregates the repeats.
//   Report.cpp      writes the evidence report and its tab-separated rows.
//   WorkloadMeasurements.cpp  holds the gate and evidence profiles and their expectations.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workload {

// One measured action. The budget is the latency a completed sample must stay within to enter
// the proposed envelope. A group of escalating sizes stops after a size misses the stall ceiling.
struct Metric {
    std::string_view name;
    std::string_view description;
    int budgetMs;
    int stallMs;
};

inline constexpr Metric kMetrics[] = {
    {"construct", "correspondence construction", 1000, 4000},
    {"edit", "correspondence edit", 1000, 4000},
    {"load", "editor load", 1000, 4000},
    {"key", "editor keystroke", 100, 400},
    {"read", "Git read", 2000, 8000},
    {"commit", "recovery commit", 500, 2000},
    {"reload", "recovery reload", 500, 2000},
};

// The Git output guard the Git read workloads and probes are sized against.
inline constexpr std::uint64_t kGitGuard = 8ull * 1024ull * 1024ull;

// The stall ceiling of a probe whose metric has no budget.
inline constexpr int kDefaultStallMs = 4000;

// The metric named `name`, or nullptr for a probe or startup record that has no budget.
inline const Metric *findMetric(std::string_view name)
{
    for (const auto &metric : kMetrics)
        if (metric.name == name) return &metric;
    return nullptr;
}

// One point of the matrix: a worker of `kind` measures `name` in `mode` at one size.
struct Spec {
    std::string group;
    std::string kind;
    std::string name;
    std::string mode;
    std::size_t lines = 0;
    std::size_t bytes = 0;
    int timeoutMs = 20000;
    int repeats = 1;
    // A larger size in the same group runs only while the smaller ones meet the stall ceiling.
    bool escalating = false;
    // The editor worker also reports its scene startup, outside the envelope.
    bool startup = false;
};

// What a worker reports for one metric of one run.
struct Record {
    std::string kind;
    std::string name;
    std::string metric;
    std::string outcome;
    std::string detail;
    std::uint64_t bytes = 0;
    std::uint64_t lines = 0;
    std::uint64_t maxLine = 0;
    std::uint64_t rssKb = 0;
    std::optional<std::uint64_t> elapsedMs;
};

// The repeats of one record name: its worst outcome and every completed sample.
struct Aggregate {
    std::string kind;
    std::string name;
    std::string metric;
    std::string group;
    std::string outcome;
    std::string detail;
    std::uint64_t bytes = 0;
    std::uint64_t lines = 0;
    std::uint64_t maxLine = 0;
    std::uint64_t rssKb = 0;
    std::vector<std::uint64_t> elapsed;
    int samples = 0;
    bool ranked = false;
};

// The arguments the supervisor passes to a worker process.
struct WorkerArgs {
    std::string kind;
    std::string name;
    std::string mode;
    std::string scratch;
    std::size_t lines = 0;
    std::size_t bytes = 0;
    int timeoutMs = 20000;
    bool startup = false;
    int argc = 0;
    char **argv = nullptr;
};

// A kind of workload: the metrics one run reports in a mode, the record a startup run adds, and
// the worker that measures it. A run whose mode has no metrics reports one record named after
// its spec, with the mode as its metric.
struct Kind {
    std::string_view name;
    std::vector<std::string_view> (*metrics)(std::string_view mode);
    std::string_view startupRecord;
    int (*worker)(const WorkerArgs &);
};

// Defined with the workers.
const Kind *findKind(std::string_view name);

// The record names one run of `spec` must report, in order.
std::vector<std::string> recordNames(const Spec &spec);
// The metric of a record that one run of `spec` reported under `name`.
std::string metricOf(const std::string &name, const Spec &spec);

// Worker side of the line format.
void emitRecord(const Record &record);
// Supervisor side: the record on one output line, or nothing for any other line.
std::optional<Record> parseRecord(const std::string &line);

// Helpers the workers share.
std::string sanitize(std::string text);
std::uint64_t vmHwmKb();
std::uint64_t since(std::chrono::steady_clock::time_point start);
Record baseRecord(const std::string &kind, const std::string &name, const std::string &metric, std::uint64_t bytes,
                  std::uint64_t lines, std::uint64_t maxLine);
void inspect(const std::string &bytes, std::uint64_t &lineCount, std::uint64_t &maxLine);

// The middle sample, or nothing without samples.
std::optional<std::uint64_t> medianOf(std::vector<std::uint64_t> values);

// Runs every spec in its own worker processes and aggregates the repeats. A cleanup failure is
// left in `problem`.
std::vector<Aggregate> measure(const std::vector<Spec> &specs, std::string *problem);

// Writes the evidence report to `path` and its rows next to it with a .tsv extension.
void writeReports(const std::string &path, const std::vector<Aggregate> &results, const std::string &cleanup);

// The worker entry point, selected by --worker on the command line.
int workerMain(int argc, char **argv);

} // namespace workload
