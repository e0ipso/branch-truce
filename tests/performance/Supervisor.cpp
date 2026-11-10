// The supervisor runs each worker in its own process, kills it at the resident-set ceiling or its
// wall-clock ceiling, reads its records and folds the repeats of each record into one aggregate.
#include "fixtures/performance/WorkloadText.h"
#include "performance/Workload.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <poll.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace workload {
namespace {

constexpr long kRssKillKb = 1536L * 1024L;
constexpr char kScratchName[] = "/truce-workload-";

std::string selfPath()
{
    char buffer[4096];
    const auto length = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (length <= 0) return {};
    buffer[length] = '\0';
    return buffer;
}

std::string makeScratch()
{
    std::string path = scratchBase() + kScratchName + "XXXXXX";
    if (mkdtemp(path.data()) == nullptr) return {};
    return path;
}

void removeScratch(const std::string &path, std::vector<std::string> *failures)
{
    if (path.rfind(scratchBase() + kScratchName, 0) != 0) {
        failures->push_back(path + " refused");
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) failures->push_back(path + " " + error.message());
}

long currentRssKb(pid_t pid)
{
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            long value = 0;
            status >> value;
            return value;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

int outcomeRank(const std::string &outcome)
{
    static const char *order[] = {
        "crashed",   "inconsistent",   "fabricated-success", "unsupported-resource", "unsupported-timeout",
        "truncated", "unacknowledged", "explicit-failure",   "incomplete",           "not-attempted",
        "completed"};
    for (int i = 0; i < 11; ++i)
        if (outcome == order[i]) return i;
    return -1;
}

std::uint64_t maxLineOf(const Spec &spec)
{
    return spec.mode == "long" ? spec.bytes : test_support::kWorkloadLineWidth;
}

// Ends every process in the worker's group, such as a Git child of a killed worker, so nothing
// writes into the scratch directory while it is removed, then reaps the worker. Only an unreaped
// worker keeps its process ID, and with it the group ID, from being reused, so this runs before
// the worker is reaped.
int endGroup(pid_t group)
{
    if (::kill(-group, SIGKILL) != 0 && errno != ESRCH) ::kill(group, SIGKILL);
    int status = 0;
    while (::waitpid(group, &status, 0) == -1 && errno == EINTR) {}
    return status;
}

// Whether the worker has exited, without reaping it.
bool exited(pid_t pid)
{
    siginfo_t info{};
    return ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid;
}

// One run of `spec` in a worker process: the records it reported, with a record for each
// expected name it did not report when it was killed or crashed. Cleanup failures are appended
// to `problems`.
std::vector<Record> runOnce(const Spec &spec, const std::string &executable, std::vector<std::string> *problems)
{
    const std::string scratch = makeScratch();
    if (scratch.empty()) {
        problems->push_back("could not create a scratch directory in " + scratchBase());
        return {};
    }
    std::vector<std::string> args{executable,
                                  "--worker",
                                  spec.kind,
                                  "--name",
                                  spec.name,
                                  "--mode",
                                  spec.mode,
                                  "--scratch",
                                  scratch,
                                  "--timeout-ms",
                                  std::to_string(spec.timeoutMs),
                                  "--lines",
                                  std::to_string(spec.lines),
                                  "--bytes",
                                  std::to_string(spec.bytes)};
    if (spec.startup) args.emplace_back("--startup");
    std::vector<char *> pointers;
    pointers.reserve(args.size() + 1);
    for (auto &arg : args) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        problems->push_back("pipe failed");
        removeScratch(scratch, problems);
        return {};
    }
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        // The worker leads its own process group, so a kill reaches the Git processes it starts,
        // and it dies with the supervisor, which a CTest timeout kills.
        setpgid(0, 0);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(127);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        setenv("QT_QPA_PLATFORM", "offscreen", 1);
        setenv("QT_QUICK_BACKEND", "software", 1);
        execv(executable.c_str(), pointers.data());
        _exit(127);
    }
    if (pid > 0) setpgid(pid, pid);
    ::close(pipefd[1]);
    if (pid < 0) {
        ::close(pipefd[0]);
        problems->push_back("fork failed");
        removeScratch(scratch, problems);
        return {};
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    std::string output;
    bool resource = false;
    bool timedOut = false;
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        const bool done = exited(pid);
        char buffer[1024];
        const ssize_t count = ::read(pipefd[0], buffer, sizeof buffer);
        if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
        if (done) break;
        if (currentRssKb(pid) > kRssKillKb) {
            resource = true;
            break;
        }
        if (since(started) > static_cast<std::uint64_t>(spec.timeoutMs)) {
            timedOut = true;
            break;
        }
        pollfd ready{pipefd[0], POLLIN, 0};
        poll(&ready, 1, 50);
    }
    const int status = endGroup(pid);
    char buffer[1024];
    ssize_t count = 0;
    while ((count = ::read(pipefd[0], buffer, sizeof buffer)) > 0)
        output.append(buffer, static_cast<std::size_t>(count));
    ::close(pipefd[0]);
    removeScratch(scratch, problems);
    // A worker that reported and then failed on its own, by exit status or by a signal the
    // supervisor did not send, cannot vouch for what it reported.
    std::string failedExit;
    if (!resource && !timedOut) {
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            failedExit = "worker exited " + std::to_string(WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            failedExit = "worker ended by signal " + std::to_string(WTERMSIG(status));
    }
    std::vector<Record> records;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        auto record = parseRecord(line);
        if (!record) continue;
        record->metric = metricOf(record->name, spec);
        if (!failedExit.empty() && record->outcome == "completed") {
            record->outcome = "inconsistent";
            record->detail = failedExit + " after reporting: " + record->detail;
        }
        records.push_back(*record);
    }
    if (resource || timedOut || records.empty()) {
        const char *outcome = resource ? "unsupported-resource" : timedOut ? "unsupported-timeout" : "crashed";
        const char *detail = resource ? "killed after resident set exceeded 1536 MiB"
            : timedOut                ? "killed after the measurement ceiling"
                                      : "no measurement record";
        for (const auto &expected : recordNames(spec)) {
            const bool present = std::any_of(records.begin(), records.end(),
                                             [&](const Record &record) { return record.name == expected; });
            if (present) continue;
            Record record;
            record.kind = spec.kind;
            record.name = expected;
            record.metric = metricOf(expected, spec);
            record.outcome = outcome;
            record.bytes = spec.bytes;
            record.lines = spec.lines;
            record.maxLine = maxLineOf(spec);
            record.detail = std::string(detail) + " status=" + std::to_string(status);
            records.push_back(record);
        }
    }
    return records;
}

bool overStall(const Aggregate &item)
{
    if (item.metric == "startup") return false;
    if (item.outcome != "completed") return true;
    const auto mid = medianOf(item.elapsed);
    if (!mid) return true;
    const auto *metric = findMetric(item.metric);
    return *mid > static_cast<std::uint64_t>(metric ? metric->stallMs : kDefaultStallMs);
}

bool nameInSpec(const std::string &name, const Spec &spec)
{
    const auto names = recordNames(spec);
    return std::find(names.begin(), names.end(), name) != names.end();
}
} // namespace

std::string scratchBase()
{
    std::error_code error;
    if (const char *chosen = std::getenv("BRANCH_TRUCE_WORKLOAD_SCRATCH");
        chosen && chosen[0] == '/' && std::filesystem::is_directory(chosen, error))
        return chosen;
    return "/var/tmp";
}

std::vector<Aggregate> measure(const std::vector<Spec> &specs, std::vector<std::string> *problems)
{
    const std::string executable = selfPath();
    std::map<std::string, bool> halted;
    std::vector<Aggregate> results;
    auto findOrAdd = [&](const Record &record, const std::string &group) -> Aggregate & {
        for (auto &item : results)
            if (item.name == record.name) return item;
        Aggregate created;
        created.kind = record.kind;
        created.name = record.name;
        created.metric = record.metric;
        created.group = group;
        created.outcome = "completed";
        created.bytes = record.bytes;
        created.lines = record.lines;
        created.maxLine = record.maxLine;
        results.push_back(created);
        return results.back();
    };
    for (const auto &spec : specs) {
        if (spec.escalating && halted[spec.group]) {
            for (const auto &expected : recordNames(spec)) {
                Record record;
                record.kind = spec.kind;
                record.name = expected;
                record.metric = metricOf(expected, spec);
                record.outcome = "not-attempted";
                record.lines = spec.lines;
                record.bytes = spec.bytes;
                record.maxLine = maxLineOf(spec);
                record.detail = "not attempted because a smaller workload in this group missed the stall rule";
                auto &item = findOrAdd(record, spec.group);
                item.outcome = "not-attempted";
                item.ranked = true;
                item.detail = record.detail;
                item.samples = 0;
            }
            continue;
        }
        bool stall = false;
        for (int repeat = 0; repeat < spec.repeats; ++repeat) {
            const auto records = runOnce(spec, executable, problems);
            for (const auto &record : records) {
                auto &item = findOrAdd(record, spec.group);
                if (!item.ranked || outcomeRank(record.outcome) < outcomeRank(item.outcome))
                    item.outcome = record.outcome;
                item.ranked = true;
                item.detail = record.detail;
                item.bytes = std::max(item.bytes, record.bytes);
                item.lines = std::max(item.lines, record.lines);
                item.maxLine = std::max(item.maxLine, record.maxLine);
                item.rssKb = std::max(item.rssKb, record.rssKb);
                if (record.elapsedMs && record.outcome == "completed") item.elapsed.push_back(*record.elapsedMs);
                if (record.outcome != "completed") stall = true;
            }
        }
        for (auto &item : results) {
            if (nameInSpec(item.name, spec)) {
                item.samples = spec.repeats;
                if (spec.escalating && overStall(item)) stall = true;
            }
        }
        if (spec.escalating && stall) halted[spec.group] = true;
    }
    return results;
}

} // namespace workload
