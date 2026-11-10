// The workers: each one generates its workload, measures one product path in its own process and
// reports a record per metric. A worker never reports a truncated or unfinished action as
// completed.
#include "core/Correspondence.h"
#include "core/FileSnapshot.h"
#include "fixtures/FixtureGit.h"
#include "fixtures/performance/WorkloadText.h"
#include "git/GitAdapter.h"
#include "performance/Workload.h"
#include "storage/RecoveryStore.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <system_error>
#include <vector>

namespace workload {
namespace {

using test_support::workloadFile;

// The Git the fixtures run; see test_support::fixtureGit().
QString fixtureGit() { return QString::fromStdString(test_support::fixtureGit()); }
using test_support::workloadLongLine;

bool covers(const std::vector<branch_truce::Passage> &passages, std::string_view source, std::string_view result)
{
    std::size_t sourceAt = 0;
    std::size_t resultAt = 0;
    if (passages.empty()) return source.empty() && result.empty();
    for (const auto &passage : passages) {
        if (passage.source.begin != sourceAt || passage.result.begin != resultAt) return false;
        if (passage.source.end < passage.source.begin || passage.result.end < passage.result.begin) return false;
        if (passage.source.end > source.size() || passage.result.end > result.size()) return false;
        sourceAt = passage.source.end;
        resultAt = passage.result.end;
    }
    return sourceAt == source.size() && resultAt == result.size();
}

branch_truce::SourceSnapshot sourceOf(std::string text, const char *id)
{
    branch_truce::SourceSnapshot source;
    source.objectId = id;
    source.mode = "100644";
    source.rawBytes = text;
    source.worktree.utf8 = std::move(text);
    source.worktree.bytes = source.worktree.utf8;
    source.worktree.format.finalNewline = !source.worktree.utf8.empty() && source.worktree.utf8.back() == '\n';
    source.label = id;
    return source;
}

bool mapCovers(const branch_truce::CorrespondenceMap &map, const std::string &earlier, const std::string &stage2,
               const std::string &stage3, const std::string &result)
{
    const auto check = [&](branch_truce::SourceSide side, const std::string &source) {
        const auto *passages = map.passages(side);
        return passages != nullptr && covers(*passages, source, map.text());
    };
    return map.text() == result && check(branch_truce::SourceSide::Earlier, earlier)
        && check(branch_truce::SourceSide::Stage2, stage2) && check(branch_truce::SourceSide::Stage3, stage3);
}

// Builds the correspondence of three sources and a result, then realigns it after a one-byte edit.
int correspondenceWorker(const WorkerArgs &args)
{
    const auto &name = args.name;
    const bool identical = args.mode == "identical";
    const bool longLine = args.mode == "long";
    const std::string earlier = longLine ? workloadLongLine(args.bytes, 'a') : workloadFile(args.lines, -1);
    const std::string stage2 =
        longLine ? workloadLongLine(args.bytes, 'b') : workloadFile(args.lines, identical ? -1 : 0);
    const std::string stage3 =
        longLine ? workloadLongLine(args.bytes, 'c') : workloadFile(args.lines, identical ? -1 : 1);
    const std::string result =
        longLine ? workloadLongLine(args.bytes, 'd') : workloadFile(args.lines, identical ? -1 : 2);
    std::uint64_t lineCount = 0;
    std::uint64_t maxLine = 0;
    inspect(result, lineCount, maxLine);
    const auto byteCount = static_cast<std::uint64_t>(result.size());
    branch_truce::FileSnapshots snapshots;
    snapshots.path = "conflict.txt";
    snapshots.provenance.sessionId = "workload";
    snapshots.provenance.operation = branch_truce::SnapshotProvenance::Operation::Merge;
    snapshots.resultMode = "100644";
    snapshots.earlier = sourceOf(earlier, "earlier");
    snapshots.stage2 = sourceOf(stage2, "stage2");
    snapshots.stage3 = sourceOf(stage3, "stage3");
    snapshots.launchResult.text = snapshots.stage2->worktree;
    const auto record = [&](const char *metric) {
        return baseRecord("correspondence", name + "-" + metric, metric, byteCount, lineCount, maxLine);
    };
    const auto failBoth = [&](const std::string &outcome, const std::string &detail) {
        for (const char *metric : {"construct", "edit"}) {
            auto failed = record(metric);
            failed.outcome = outcome;
            failed.detail = detail;
            emitRecord(failed);
        }
    };
    bool emittedConstruct = false;
    try {
        const auto started = std::chrono::steady_clock::now();
        branch_truce::CorrespondenceMap map(1, std::make_shared<const branch_truce::FileSnapshots>(snapshots), result);
        auto construct = record("construct");
        construct.elapsedMs = since(started);
        construct.rssKb = vmHwmKb();
        if (!mapCovers(map, earlier, stage2, stage3, result)) {
            construct.outcome = "truncated";
            construct.detail = "passage partition does not cover the buffers";
            emitRecord(construct);
            auto edit = record("edit");
            edit.outcome = "incomplete";
            edit.detail = "skipped after a truncated construction";
            emitRecord(edit);
            return 0;
        }
        construct.outcome = "completed";
        construct.detail = identical ? "identical buffers take the direct path" : "three differing sources";
        emitRecord(construct);
        emittedConstruct = true;
        const auto editStarted = std::chrono::steady_clock::now();
        const auto edited = map.edited(2, branch_truce::ResultEdit{branch_truce::ByteRange{0, 0}, "X"});
        auto edit = record("edit");
        edit.elapsedMs = since(editStarted);
        edit.rssKb = vmHwmKb();
        const std::string expected = "X" + result;
        if (edited.text() != expected || !mapCovers(edited, earlier, stage2, stage3, expected)) {
            edit.outcome = "truncated";
            edit.detail = "edit result or passage partition is incomplete";
        } else {
            edit.outcome = "completed";
            edit.detail = "one-byte insertion realigned all sources";
        }
        emitRecord(edit);
    } catch (const std::exception &error) {
        if (!emittedConstruct)
            failBoth("explicit-failure", error.what());
        else {
            auto edit = record("edit");
            edit.outcome = "explicit-failure";
            edit.detail = error.what();
            emitRecord(edit);
        }
    }
    return 0;
}

const char *gitOutcome(branch_truce::git::Outcome outcome)
{
    using branch_truce::git::Outcome;
    switch (outcome) {
    case Outcome::Success: return "Success";
    case Outcome::Rejected: return "Rejected";
    case Outcome::LaunchFailure: return "LaunchFailure";
    case Outcome::ExitFailure: return "ExitFailure";
    case Outcome::ProcessFailure: return "ProcessFailure";
    case Outcome::Cancelled: return "Cancelled";
    case Outcome::TimedOut: return "TimedOut";
    case Outcome::OutputLimit: return "OutputLimit";
    case Outcome::InspectionFailure: return "InspectionFailure";
    }
    return "Unknown";
}

QProcessEnvironment gitEnvironment(const QString &home)
{
    QProcessEnvironment env;
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    env.insert(QStringLiteral("HOME"), home);
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/config"));
    env.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    env.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    env.insert(QStringLiteral("GIT_CONFIG_SYSTEM"), QStringLiteral("/dev/null"));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    return env;
}

bool runGit(const QString &repo, const QString &home, const QStringList &arguments, const QByteArray &input,
            QByteArray *output, QString *error)
{
    QProcess process;
    process.setProgram(fixtureGit());
    process.setArguments(arguments);
    process.setWorkingDirectory(repo);
    process.setProcessEnvironment(gitEnvironment(home));
    process.start();
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("git did not start");
        return false;
    }
    if (process.write(input) != static_cast<qint64>(input.size())) {
        *error = QStringLiteral("git stdin write was short");
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(1000);
        *error = QStringLiteral("git timed out");
        return false;
    }
    *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QString::fromLocal8Bit(process.readAllStandardError());
        return false;
    }
    return true;
}

std::uint64_t gitChildRssKb()
{
    std::uint64_t peak = 0;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
        std::ifstream children((entry.path() / "children").string());
        long pid = 0;
        while (children >> pid) {
            if (pid <= 0) continue;
            std::ifstream status("/proc/" + std::to_string(pid) + "/status");
            std::string key;
            while (status >> key) {
                if (key == "VmRSS:") {
                    long value = 0;
                    status >> value;
                    if (value > 0) peak = std::max(peak, static_cast<std::uint64_t>(value));
                }
                status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
        }
    }
    return peak;
}

// Reads a blob of the given size back through GitAdapter, or in "input" mode hands GitAdapter an
// input over its limit.
int gitWorker(const WorkerArgs &args)
{
    const auto &name = args.name;
    const auto &mode = args.mode;
    const int budgetMs = std::max(1000, args.timeoutMs - 1000);
    int argc = args.argc;
    QCoreApplication app(argc, args.argv);
    const QString home = QString::fromStdString(args.scratch);
    const QString repo = home + QStringLiteral("/repo");
    QDir().mkpath(repo);
    QByteArray ignored;
    QString error;
    if (!runGit(repo, home,
                {QStringLiteral("init"), QStringLiteral("--quiet"), QStringLiteral("--initial-branch=main")}, {},
                &ignored, &error)) {
        auto record = baseRecord("git", name, mode, args.bytes, 1, args.bytes);
        record.outcome = "explicit-failure";
        record.detail = "git init failed: " + error.toStdString();
        emitRecord(record);
        return 0;
    }
    QByteArray payload(static_cast<qsizetype>(args.bytes), 'g');
    if (!payload.isEmpty()) payload[payload.size() - 1] = 'G';
    auto record = baseRecord("git", name, mode, static_cast<std::uint64_t>(payload.size()), 1,
                             static_cast<std::uint64_t>(payload.size()));
    using branch_truce::git::GitAdapter;
    using branch_truce::git::Operation;
    using branch_truce::git::Outcome;
    using branch_truce::git::Request;
    using branch_truce::git::Result;
    GitAdapter adapter(fixtureGit());
    Request request;
    request.directory = repo;
    request.sessionGeneration = 1;
    request.inputRevision = 1;
    if (mode == "input") {
        request.operation = Operation::CanonicalizeText;
        request.path = "blob.txt";
        request.input = payload;
    } else {
        QByteArray oid;
        if (!runGit(repo, home, {QStringLiteral("hash-object"), QStringLiteral("-w"), QStringLiteral("--stdin")},
                    payload, &oid, &error)) {
            record.outcome = "explicit-failure";
            record.detail = "hash-object failed: " + error.toStdString();
            emitRecord(record);
            return 0;
        }
        oid = oid.trimmed();
        request.operation = Operation::ReadObject;
        request.objectId = oid;
    }
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool finished = false;
    Result result;
    std::uint64_t childRss = 0;
    QObject::connect(&adapter, &GitAdapter::finished, &loop, [&](const Result &value) {
        result = value;
        finished = true;
        loop.quit();
    });
    QTimer sampler;
    QObject::connect(&sampler, &QTimer::timeout, [&] { childRss = std::max(childRss, gitChildRssKb()); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto started = std::chrono::steady_clock::now();
    sampler.start(5);
    if (!adapter.start(request)) {
        record.outcome = "explicit-failure";
        record.detail = "GitAdapter refused the request";
        emitRecord(record);
        return 0;
    }
    deadline.start(std::max(1000, budgetMs));
    loop.exec();
    sampler.stop();
    childRss = std::max(childRss, gitChildRssKb());
    record.elapsedMs = since(started);
    record.rssKb = vmHwmKb();
    const auto captured = static_cast<std::uint64_t>(std::max<qsizetype>(result.standardOutput.size(), 0));
    const auto expected = static_cast<std::uint64_t>(payload.size());
    std::ostringstream detail;
    detail << "api=" << gitOutcome(result.outcome) << " incomplete=" << (result.outputIncomplete ? 1 : 0)
           << " captured=" << captured << " expected=" << expected << " exit=" << result.exitCode
           << " git_rss_kb=" << (childRss > 0 ? std::to_string(childRss) : std::string("-"))
           << " diagnostic=" << sanitize(result.diagnostic.toStdString());
    record.detail = detail.str();
    if (!finished) {
        record.outcome = "incomplete";
        record.detail += " worker budget expired";
    } else if (result.outcome == Outcome::Success && !result.outputIncomplete && result.standardOutput == payload) {
        record.outcome = "completed";
    } else if (result.outcome == Outcome::Success) {
        record.outcome = "fabricated-success";
    } else {
        record.outcome = "explicit-failure";
    }
    emitRecord(record);
    return 0;
}

// Commits a payload through the recovery store and reads it back. "bad-digest" names the payload
// with a wrong digest and "overload" commits under a 256 KiB file-size limit; both must fail.
int recoveryWorker(const WorkerArgs &args)
{
    const auto &name = args.name;
    const auto &mode = args.mode;
    int argc = args.argc;
    QCoreApplication app(argc, args.argv);
    const QString directory = QString::fromStdString(args.scratch) + QStringLiteral("/recovery");
    branch_truce::storage::RecoveryStore store(directory);
    const bool overload = mode == "overload";
    const bool badDigest = mode == "bad-digest";
    const std::size_t payloadBytes = overload ? (1024ull * 1024ull) : badDigest ? 64 : args.bytes;
    QByteArray payload(static_cast<qsizetype>(payloadBytes), 'r');
    if (!payload.isEmpty()) payload[0] = '\0';
    const auto hash = branch_truce::storage::RecoveryStore::digest(payload);
    branch_truce::storage::RecoveryTransaction transaction;
    transaction.id = QStringLiteral("measurement");
    transaction.state = QJsonObject{{QStringLiteral("measurement"), true}};
    transaction.payloads.insert(badDigest ? QByteArray(64, '0') : hash, payload);
    if (overload) {
        if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
            auto record = baseRecord("recovery", name, mode, payloadBytes, 1, payloadBytes);
            record.outcome = "explicit-failure";
            record.detail = "could not ignore SIGXFSZ";
            emitRecord(record);
            return 0;
        }
        struct rlimit limit{};
        limit.rlim_cur = 256 * 1024;
        limit.rlim_max = 256 * 1024;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            auto record = baseRecord("recovery", name, mode, payloadBytes, 1, payloadBytes);
            record.outcome = "explicit-failure";
            record.detail = "setrlimit failed";
            emitRecord(record);
            return 0;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    const auto lease = branch_truce::storage::RecoveryLease::acquire(directory);
    const auto committed = lease
        ? lease->commit(transaction)
        : branch_truce::storage::RecoveryCommit{false, QStringLiteral("recovery store is held")};
    const auto commitMs = since(started);
    const auto listing = store.read();
    QString payloadError;
    const auto loaded = store.payload(hash, &payloadError);
    const bool acknowledged =
        committed.succeeded && listing.succeeded && listing.transactions.size() == 1 && loaded == payload;
    auto record = baseRecord("recovery", name, mode == "commit" ? "commit" : mode,
                             static_cast<std::uint64_t>(payload.size()), 1, static_cast<std::uint64_t>(payload.size()));
    record.elapsedMs = commitMs;
    record.rssKb = vmHwmKb();
    std::ostringstream detail;
    detail << "acknowledged=" << (acknowledged ? 1 : 0) << " transactions=" << listing.transactions.size()
           << " incompleteTail=" << (listing.incompleteTail ? 1 : 0)
           << " error=" << sanitize((committed.succeeded ? payloadError : committed.error).toStdString());
    record.detail = detail.str();
    if (badDigest || overload) {
        record.outcome = acknowledged ? "fabricated-success" : "explicit-failure";
        emitRecord(record);
        return 0;
    }
    if (!committed.succeeded) {
        record.outcome = "explicit-failure";
        emitRecord(record);
        auto reload = record;
        reload.name = name + "-reload";
        reload.metric = "reload";
        reload.elapsedMs = std::nullopt;
        emitRecord(reload);
        return 0;
    }
    record.outcome = acknowledged ? "completed" : "fabricated-success";
    record.name = name + "-commit";
    record.metric = "commit";
    emitRecord(record);
    const auto reloadStarted = std::chrono::steady_clock::now();
    branch_truce::storage::RecoveryStore again(directory);
    const auto reread = again.read();
    QString againError;
    const auto againBytes = again.payload(hash, &againError);
    auto reload = baseRecord("recovery", name + "-reload", "reload", static_cast<std::uint64_t>(payload.size()), 1,
                             static_cast<std::uint64_t>(payload.size()));
    reload.elapsedMs = since(reloadStarted);
    reload.rssKb = vmHwmKb();
    const bool roundTrip = reread.succeeded && reread.transactions.size() == 1 && againBytes == payload;
    reload.outcome = roundTrip ? "completed" : "truncated";
    reload.detail =
        roundTrip ? "journal and payload match the acknowledged bytes" : "reload differed from the acknowledged bytes";
    emitRecord(reload);
    return 0;
}

// Reports a completed record and then dies on SIGTERM, as a worker does that crashes in teardown
// after its measurement. The supervisor must not count that record as completed.
int selfCheckWorker(const WorkerArgs &args)
{
    auto record = baseRecord("self-check", args.name, "signal", 0, 0, 0);
    record.elapsedMs = 0;
    record.outcome = "completed";
    record.detail = "reported before the worker was signalled";
    emitRecord(record);
    std::signal(SIGTERM, SIG_DFL);
    std::raise(SIGTERM);
    return 0;
}

std::vector<std::string_view> correspondenceMetrics(std::string_view) { return {"construct", "edit"}; }
std::vector<std::string_view> editorMetrics(std::string_view) { return {"load", "key"}; }
std::vector<std::string_view> singleRecord(std::string_view) { return {}; }
std::vector<std::string_view> recoveryMetrics(std::string_view mode)
{
    if (mode == "commit") return {"commit", "reload"};
    return {};
}

const Kind kKinds[] = {
    {"correspondence", correspondenceMetrics, {}, correspondenceWorker},
    {"editor", editorMetrics, "editor-startup", editorWorker},
    {"git", singleRecord, {}, gitWorker},
    {"recovery", recoveryMetrics, {}, recoveryWorker},
    {"self-check", singleRecord, {}, selfCheckWorker},
};

WorkerArgs parseWorkerArgs(int argc, char **argv)
{
    WorkerArgs args;
    args.argc = argc;
    args.argv = argv;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto take = [&](std::string *value) {
            if (i + 1 < argc) *value = argv[++i];
        };
        if (key == "--worker")
            take(&args.kind);
        else if (key == "--name")
            take(&args.name);
        else if (key == "--mode")
            take(&args.mode);
        else if (key == "--scratch")
            take(&args.scratch);
        else if (key == "--lines" && i + 1 < argc)
            args.lines = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (key == "--bytes" && i + 1 < argc)
            args.bytes = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (key == "--timeout-ms" && i + 1 < argc)
            args.timeoutMs = std::atoi(argv[++i]);
        else if (key == "--startup")
            args.startup = true;
    }
    return args;
}
} // namespace

const Kind *findKind(std::string_view name)
{
    for (const auto &kind : kKinds)
        if (kind.name == name) return &kind;
    return nullptr;
}

int workerMain(int argc, char **argv)
{
    const auto args = parseWorkerArgs(argc, argv);
    if (const auto *kind = findKind(args.kind)) return kind->worker(args);
    std::cerr << "unknown worker\n";
    return 2;
}

} // namespace workload
