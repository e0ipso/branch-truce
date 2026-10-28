// Generates representative files and adverse long lines, measures the product
// paths, and refuses to report a truncated or unfinished action as completed.
#include "git/GitAdapter.h"
#include "storage/RecoveryStore.h"
#include "core/Correspondence.h"
#include "core/FileSnapshot.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QTextDocument>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#ifndef BRANCH_TRUCE_MEASUREMENT_BUILD
#define BRANCH_TRUCE_MEASUREMENT_BUILD "unspecified"
#endif

namespace {
constexpr int kKeyBudgetMs = 100;
constexpr int kLoadBudgetMs = 1000;
constexpr int kCorrespondenceBudgetMs = 1000;
constexpr int kGitBudgetMs = 2000;
constexpr int kRecoveryBudgetMs = 500;
constexpr int kKeyStallMs = 400;
constexpr int kLayoutStallMs = 4000;
constexpr int kGitStallMs = 8000;
constexpr int kRecoveryStallMs = 2000;
constexpr std::uint64_t kGitGuard = 8ull * 1024ull * 1024ull;
constexpr long kRssKillKb = 1536L * 1024L;
constexpr std::size_t kWidth = 96;
constexpr char kScratchPrefix[] = "/var/tmp/truce-workload-";

int gArgc = 0;
char **gArgv = nullptr;

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

struct Spec {
    const char *group;
    const char *kind;
    const char *name;
    const char *mode;
    std::size_t lines = 0;
    std::size_t bytes = 0;
    int timeoutMs = 20000;
    int repeats = 1;
    bool escalating = false;
    bool startup = false;
};

std::string sanitize(std::string text)
{
    for (char &c : text) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    if (text.size() > 240) text.resize(240);
    return text;
}

std::uint64_t vmHwmKb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            long value = 0;
            status >> value;
            return value > 0 ? static_cast<std::uint64_t>(value) : 0;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

void inspect(const std::string &bytes, std::uint64_t &lineCount, std::uint64_t &maxLine)
{
    lineCount = 0;
    maxLine = 0;
    std::uint64_t current = 0;
    for (const char c : bytes) {
        ++current;
        if (c == '\n') {
            ++lineCount;
            maxLine = std::max(maxLine, current);
            current = 0;
        }
    }
    if (current > 0 || bytes.empty()) {
        if (current > 0) ++lineCount;
        maxLine = std::max(maxLine, current);
    }
}

void emitRecord(const Record &record)
{
    std::cout << "MEASURE\t" << record.kind << '\t' << record.name << '\t' << record.outcome << '\t'
              << record.bytes << '\t' << record.lines << '\t' << record.maxLine << '\t'
              << (record.elapsedMs ? std::to_string(*record.elapsedMs) : std::string("-")) << '\t'
              << record.rssKb << '\t' << sanitize(record.detail) << '\n';
    std::cout.flush();
}

Record baseRecord(const std::string &kind, const std::string &name, const std::string &metric,
                  const std::string &bytes, std::uint64_t byteCount, std::uint64_t lineCount,
                  std::uint64_t maxLine)
{
    Record record;
    record.kind = kind;
    record.name = name;
    record.metric = metric;
    record.bytes = byteCount;
    record.lines = lineCount;
    record.maxLine = maxLine;
    record.rssKb = vmHwmKb();
    (void)bytes;
    return record;
}

std::uint64_t since(std::chrono::steady_clock::time_point start)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return static_cast<std::uint64_t>(elapsed.count() < 0 ? 0 : elapsed.count());
}

std::string makeFile(std::size_t lineCount, int variant)
{
    std::string out;
    out.reserve(lineCount * (kWidth + 16));
    for (std::size_t i = 0; i < lineCount; ++i) {
        if (i % 40 == 0) {
            out += "repeat-passage\n";
            continue;
        }
        std::string line;
        char prefix[32];
        const int wrote = std::snprintf(prefix, sizeof prefix, "L%06zu-", i);
        if (wrote > 0) line.append(prefix, static_cast<std::size_t>(wrote));
        if (i % 100 == 7) line += "caf\xc3\xa9-";
        const bool changed = variant >= 0 && (i % 25) == static_cast<std::size_t>(variant);
        if (line.size() + 2 <= kWidth) line.append(kWidth - 2 - line.size(), 'a');
        line.push_back(changed ? 'Z' : 'a');
        line.push_back('\n');
        out += line;
    }
    return out;
}

std::string makeLong(std::size_t bytes, char last)
{
    if (bytes == 0) return {};
    std::string text(bytes, 'x');
    text.back() = last;
    return text;
}

bool covers(const std::vector<branch_truce::Passage> &passages, std::string_view source,
            std::string_view result)
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

bool mapCovers(const branch_truce::CorrespondenceMap &map, const std::string &earlier,
               const std::string &stage2, const std::string &stage3, const std::string &result)
{
    const auto check = [&](branch_truce::SourceSide side, const std::string &source) {
        const auto *passages = map.passages(side);
        return passages != nullptr && covers(*passages, source, map.text());
    };
    return map.text() == result && check(branch_truce::SourceSide::Earlier, earlier)
        && check(branch_truce::SourceSide::Stage2, stage2)
        && check(branch_truce::SourceSide::Stage3, stage3);
}

int correspondenceWorker(const std::string &name, const std::string &mode, std::size_t lines, std::size_t bytes)
{
    const bool identical = mode == "identical";
    const bool longLine = mode == "long";
    const std::string earlier = longLine ? makeLong(bytes, 'a') : makeFile(lines, -1);
    const std::string stage2 = longLine ? makeLong(bytes, 'b') : makeFile(lines, identical ? -1 : 0);
    const std::string stage3 = longLine ? makeLong(bytes, 'c') : makeFile(lines, identical ? -1 : 1);
    const std::string result = longLine ? makeLong(bytes, 'd') : makeFile(lines, identical ? -1 : 2);
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
    const auto failBoth = [&](const std::string &outcome, const std::string &detail) {
        for (const char *metric : {"construct", "edit"}) {
            auto record = baseRecord("correspondence", name + "-" + metric, metric, result, byteCount, lineCount, maxLine);
            record.outcome = outcome;
            record.detail = detail;
            emitRecord(record);
        }
    };
    bool emittedConstruct = false;
    try {
        const auto started = std::chrono::steady_clock::now();
        branch_truce::CorrespondenceMap map(1, snapshots, result);
        auto record = baseRecord("correspondence", name + "-construct", "construct", result, byteCount, lineCount, maxLine);
        record.elapsedMs = since(started);
        record.rssKb = vmHwmKb();
        if (!mapCovers(map, earlier, stage2, stage3, result)) {
            record.outcome = "truncated";
            record.detail = "passage partition does not cover the buffers";
            emitRecord(record);
            auto edit = baseRecord("correspondence", name + "-edit", "edit", result, byteCount, lineCount, maxLine);
            edit.outcome = "incomplete";
            edit.detail = "skipped after a truncated construction";
            emitRecord(edit);
            return 0;
        }
        record.outcome = "completed";
        record.detail = identical ? "identical buffers take the direct path" : "three differing sources";
        emitRecord(record);
        emittedConstruct = true;
        const auto editStarted = std::chrono::steady_clock::now();
        const auto edited = map.edited(2, branch_truce::ResultEdit{branch_truce::ByteRange{0, 0}, "X"});
        auto edit = baseRecord("correspondence", name + "-edit", "edit", result, byteCount, lineCount, maxLine);
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
        if (!emittedConstruct) failBoth("explicit-failure", error.what());
        else {
            auto edit = baseRecord("correspondence", name + "-edit", "edit", result, byteCount, lineCount, maxLine);
            edit.outcome = "explicit-failure";
            edit.detail = error.what();
            emitRecord(edit);
        }
    }
    return 0;
}

QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name) return root;
    const auto children = root->childItems();
    for (auto *child : children) {
        if (auto *found = findItem(child, name)) return found;
    }
    return nullptr;
}

QByteArray rawText(QQuickItem *item)
{
    auto *document = item->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    return document->toRawText().replace(QChar(0x2029), QChar(u'\n')).toUtf8();
}

int editorWorker(const std::string &name, const std::string &mode, std::size_t lines, std::size_t bytes,
                 int budgetMs, bool startup)
{
    if (qEnvironmentVariable("QT_QPA_PLATFORM") != QLatin1String("offscreen")) {
        std::cout << "editor worker requires QT_QPA_PLATFORM=offscreen\n";
        return 2;
    }
    QGuiApplication app(gArgc, gArgv);
    const std::string text = mode == "long" ? makeLong(bytes, 'x') : makeFile(lines, 2);
    std::uint64_t lineCount = 0;
    std::uint64_t maxLine = 0;
    inspect(text, lineCount, maxLine);
    const auto byteCount = static_cast<std::uint64_t>(text.size());
    const auto fail = [&](const std::string &outcome, const std::string &detail) {
        for (const char *metric : {"load", "key"}) {
            auto record = baseRecord("editor", name + "-" + metric, metric, text, byteCount, lineCount, maxLine);
            record.outcome = outcome;
            record.detail = detail;
            emitRecord(record);
        }
    };
    QElapsedTimer sceneClock;
    sceneClock.start();
    QQuickView view;
    view.setSource(QUrl(QStringLiteral("qrc:/Editor.qml")));
    if (view.status() != QQuickView::Ready) {
        fail("explicit-failure", "editor scene failed to load");
        return 0;
    }
    view.show();
    app.processEvents();
    auto *editor = findItem(view.rootObject(), QStringLiteral("result"));
    auto *sourceA = findItem(view.rootObject(), QStringLiteral("sourceA"));
    auto *sourceB = findItem(view.rootObject(), QStringLiteral("sourceB"));
    if (startup) {
        auto record = baseRecord("editor", "editor-startup", "startup", text, 0, 0, 0);
        record.elapsedMs = static_cast<std::uint64_t>(sceneClock.elapsed());
        record.outcome = editor != nullptr && sourceA != nullptr && sourceB != nullptr ? "completed" : "explicit-failure";
        record.detail = "offscreen software scene startup, excluded from the envelope";
        record.rssKb = vmHwmKb();
        emitRecord(record);
    }
    if (editor == nullptr || sourceA == nullptr || sourceB == nullptr) {
        fail("explicit-failure", "result or source pane is missing");
        return 0;
    }
    QElapsedTimer timer;
    timer.start();
    const auto qtext = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    editor->setProperty("text", qtext);
    sourceA->setProperty("text", qtext);
    sourceB->setProperty("text", qtext);
    app.processEvents();
    const auto contentWidth = editor->property("contentWidth").toDouble();
    const auto contentHeight = editor->property("contentHeight").toDouble();
    const auto loadMs = static_cast<std::uint64_t>(timer.elapsed());
    const QByteArray loaded = rawText(editor);
    const QByteArray generated(text.data(), static_cast<qsizetype>(text.size()));
    const bool panesMatch = rawText(sourceA) == loaded && rawText(sourceB) == loaded;
    auto load = baseRecord("editor", name + "-load", "load", text, byteCount, lineCount, maxLine);
    load.elapsedMs = loadMs;
    load.rssKb = vmHwmKb();
    std::ostringstream loadDetail;
    loadDetail << "loaded=" << loaded.size() << " expected=" << generated.size()
               << " width=" << contentWidth << " height=" << contentHeight;
    if (loaded != generated || !panesMatch || !(contentWidth > 0.0) || !(contentHeight > 0.0)) {
        const bool textKept = loaded == generated && panesMatch;
        load.outcome = textKept ? "explicit-failure" : "truncated";
        load.detail = loadDetail.str() + (textKept ? " text retained but content width is not positive"
                                                   : " text differs from the generated buffer");
        emitRecord(load);
        auto key = baseRecord("editor", name + "-key", "key", text, byteCount, lineCount, maxLine);
        key.outcome = "incomplete";
        key.detail = "skipped after the load was not completed";
        emitRecord(key);
        return 0;
    }
    load.outcome = "completed";
    load.detail = "three panes, offscreen software, layout included";
    emitRecord(load);

    QElapsedTimer cursor;
    cursor.start();
    editor->forceActiveFocus();
    editor->setProperty("cursorPosition", 0);
    app.processEvents();
    if (cursor.elapsed() > budgetMs) {
        auto key = baseRecord("editor", name + "-key", "key", text, byteCount, lineCount, maxLine);
        key.outcome = "unacknowledged";
        key.detail = "cursor placement exceeded the worker budget";
        emitRecord(key);
        return 0;
    }
    timer.restart();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QGuiApplication::sendEvent(editor, &press);
    app.processEvents();
    (void)editor->property("contentWidth").toDouble();
    const auto keyMs = static_cast<std::uint64_t>(timer.elapsed());
    const QByteArray after = rawText(editor);
    const QByteArray expected = QByteArray("a") + QByteArray(text.data(), static_cast<qsizetype>(text.size()));
    auto key = baseRecord("editor", name + "-key", "key", text, byteCount, lineCount, maxLine);
    key.elapsedMs = keyMs;
    key.rssKb = vmHwmKb();
    if (after == expected) {
        key.outcome = "completed";
        key.detail = "inserted one ASCII letter at the start";
    } else if (after.size() == loaded.size()) {
        key.outcome = "unacknowledged";
        key.detail = "document length did not change";
    } else {
        key.outcome = "truncated";
        key.detail = "document changed but does not contain the full inserted text";
    }
    emitRecord(key);
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
    process.setProgram(QStringLiteral("/usr/bin/git"));
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

int gitWorker(const std::string &name, const std::string &mode, std::size_t bytes, const std::string &scratch, int budgetMs)
{
    QCoreApplication app(gArgc, gArgv);
    const QString home = QString::fromStdString(scratch);
    const QString repo = home + QStringLiteral("/repo");
    QDir().mkpath(repo);
    QByteArray ignored;
    QString error;
    if (!runGit(repo, home, {QStringLiteral("init"), QStringLiteral("--quiet"), QStringLiteral("--initial-branch=main")}, {}, &ignored, &error)) {
        auto record = baseRecord("git", name, mode, {}, bytes, 1, bytes);
        record.outcome = "explicit-failure";
        record.detail = "git init failed: " + error.toStdString();
        emitRecord(record);
        return 0;
    }
    const std::size_t payloadBytes = mode == "input" ? kGitGuard + 1 : bytes;
    QByteArray payload(static_cast<qsizetype>(payloadBytes), 'g');
    if (!payload.isEmpty()) payload[payload.size() - 1] = 'G';
    auto record = baseRecord("git", name, mode, {}, static_cast<std::uint64_t>(payload.size()), 1, static_cast<std::uint64_t>(payload.size()));
    using branch_truce::git::GitAdapter;
    using branch_truce::git::Operation;
    using branch_truce::git::Outcome;
    using branch_truce::git::Request;
    using branch_truce::git::Result;
    GitAdapter adapter(QStringLiteral("/usr/bin/git"));
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
        if (!runGit(repo, home, {QStringLiteral("hash-object"), QStringLiteral("-w"), QStringLiteral("--stdin")}, payload, &oid, &error)) {
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
    } else if (result.outcome == Outcome::OutputLimit && result.outputIncomplete && captured < expected) {
        record.outcome = "explicit-failure";
    } else {
        record.outcome = "explicit-failure";
    }
    emitRecord(record);
    return 0;
}

int recoveryWorker(const std::string &name, const std::string &mode, std::size_t bytes, const std::string &scratch)
{
    QCoreApplication app(gArgc, gArgv);
    const QString directory = QString::fromStdString(scratch) + QStringLiteral("/recovery");
    branch_truce::storage::RecoveryStore store(directory);
    const bool overload = mode == "overload";
    const bool badDigest = mode == "bad-digest";
    const std::size_t payloadBytes = overload ? (1024ull * 1024ull) : badDigest ? 64 : bytes;
    QByteArray payload(static_cast<qsizetype>(payloadBytes), 'r');
    if (!payload.isEmpty()) payload[0] = '\0';
    const auto hash = branch_truce::storage::RecoveryStore::digest(payload);
    branch_truce::storage::RecoveryTransaction transaction;
    transaction.state = QJsonObject{{QStringLiteral("schema"), 1},
        {QStringLiteral("transactionId"), QStringLiteral("measurement")}};
    transaction.payloads.insert(badDigest ? QByteArray(64, '0') : hash, payload);
    if (overload) {
        if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) {
            auto record = baseRecord("recovery", name, mode, {}, payloadBytes, 1, payloadBytes);
            record.outcome = "explicit-failure";
            record.detail = "could not ignore SIGXFSZ";
            emitRecord(record);
            return 0;
        }
        struct rlimit limit {};
        limit.rlim_cur = 256 * 1024;
        limit.rlim_max = 256 * 1024;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            auto record = baseRecord("recovery", name, mode, {}, payloadBytes, 1, payloadBytes);
            record.outcome = "explicit-failure";
            record.detail = "setrlimit failed";
            emitRecord(record);
            return 0;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    const auto committed = store.commit(transaction);
    const auto commitMs = since(started);
    const auto listing = store.read();
    QString payloadError;
    const auto loaded = store.payload(hash, &payloadError);
    const bool acknowledged = committed.succeeded && listing.succeeded && listing.transactions.size() == 1
        && loaded == payload;
    auto record = baseRecord("recovery", name, mode == "commit" ? "commit" : mode, {},
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
    auto reload = baseRecord("recovery", name + "-reload", "reload", {},
                             static_cast<std::uint64_t>(payload.size()), 1, static_cast<std::uint64_t>(payload.size()));
    reload.elapsedMs = since(reloadStarted);
    reload.rssKb = vmHwmKb();
    const bool roundTrip = reread.succeeded && reread.transactions.size() == 1 && againBytes == payload;
    reload.outcome = roundTrip ? "completed" : "truncated";
    reload.detail = roundTrip ? "journal and payload match the acknowledged bytes" : "reload differed from the acknowledged bytes";
    emitRecord(reload);
    return 0;
}

struct Args {
    std::string kind;
    std::string name;
    std::string mode;
    std::string scratch;
    std::size_t lines = 0;
    std::size_t bytes = 0;
    int timeoutMs = 20000;
    bool startup = false;
};

Args parseArgs(int argc, char **argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto take = [&](std::string *value) {
            if (i + 1 < argc) *value = argv[++i];
        };
        if (key == "--worker") take(&args.kind);
        else if (key == "--name") take(&args.name);
        else if (key == "--mode") take(&args.mode);
        else if (key == "--scratch") take(&args.scratch);
        else if (key == "--lines" && i + 1 < argc) args.lines = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (key == "--bytes" && i + 1 < argc) args.bytes = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (key == "--timeout-ms" && i + 1 < argc) args.timeoutMs = std::atoi(argv[++i]);
        else if (key == "--startup") args.startup = true;
    }
    return args;
}

int workerMain(int argc, char **argv)
{
    const Args args = parseArgs(argc, argv);
    if (args.kind == "correspondence") return correspondenceWorker(args.name, args.mode, args.lines, args.bytes);
    if (args.kind == "editor") {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) setenv("QT_QPA_PLATFORM", "offscreen", 0);
        if (qgetenv("QT_QUICK_BACKEND").isEmpty()) setenv("QT_QUICK_BACKEND", "software", 0);
        return editorWorker(args.name, args.mode, args.lines, args.bytes, std::max(1000, args.timeoutMs - 1000), args.startup);
    }
    if (args.kind == "git") return gitWorker(args.name, args.mode, args.bytes, args.scratch, std::max(1000, args.timeoutMs - 1000));
    if (args.kind == "recovery") return recoveryWorker(args.name, args.mode, args.bytes, args.scratch);
    std::cerr << "unknown worker\n";
    return 2;
}

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
    std::string path = std::string(kScratchPrefix) + "XXXXXX";
    if (mkdtemp(path.data()) == nullptr) return {};
    return path;
}

void removeScratch(const std::string &path, std::vector<std::string> *failures)
{
    if (path.rfind(kScratchPrefix, 0) != 0) {
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

std::vector<std::string> splitTab(const std::string &line)
{
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == '\t') {
            fields.push_back(current);
            current.clear();
        } else current.push_back(c);
    }
    fields.push_back(current);
    return fields;
}

int outcomeRank(const std::string &outcome)
{
    static const char *order[] = {"crashed", "inconsistent", "fabricated-success", "unsupported-resource",
        "unsupported-timeout", "truncated", "unacknowledged", "explicit-failure", "incomplete",
        "not-attempted", "completed"};
    for (int i = 0; i < 11; ++i) if (outcome == order[i]) return i;
    return -1;
}

std::optional<std::uint64_t> medianOf(std::vector<std::uint64_t> values)
{
    if (values.empty()) return std::nullopt;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::string metricOf(const std::string &name, const char *mode)
{
    static const char *suffixes[] = {"construct", "edit", "load", "key", "commit", "reload", "startup"};
    for (const char *suffix : suffixes) {
        const std::string tail = std::string("-") + suffix;
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) return suffix;
    }
    if (name == "editor-startup") return "startup";
    return mode;
}

std::vector<std::string> recordNames(const Spec &spec)
{
    const std::string base = spec.name;
    if (std::string(spec.kind) == "correspondence") return {base + "-construct", base + "-edit"};
    if (std::string(spec.kind) == "editor") {
        std::vector<std::string> names;
        if (spec.startup) names.emplace_back("editor-startup");
        names.push_back(base + "-load");
        names.push_back(base + "-key");
        return names;
    }
    if (std::string(spec.kind) == "recovery" && std::string(spec.mode) == "commit")
        return {base + "-commit", base + "-reload"};
    return {base};
}

bool nameInSpec(const std::string &name, const Spec &spec)
{
    for (const auto &expected : recordNames(spec)) if (expected == name) return true;
    return false;
}

std::vector<Record> runOnce(const Spec &spec, const std::string &executable, std::string *problem)
{
    const std::string scratch = makeScratch();
    if (scratch.empty()) {
        *problem = "could not create /var/tmp scratch";
        return {};
    }
    std::vector<std::string> args{executable, "--worker", spec.kind, "--name", spec.name, "--mode", spec.mode,
        "--scratch", scratch, "--timeout-ms", std::to_string(spec.timeoutMs),
        "--lines", std::to_string(spec.lines), "--bytes", std::to_string(spec.bytes)};
    if (spec.startup) args.emplace_back("--startup");
    std::vector<char *> pointers;
    pointers.reserve(args.size() + 1);
    for (auto &arg : args) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        std::vector<std::string> failures;
        removeScratch(scratch, &failures);
        *problem = "pipe failed";
        return {};
    }
    const pid_t pid = fork();
    if (pid == 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        setenv("QT_QPA_PLATFORM", "offscreen", 1);
        setenv("QT_QUICK_BACKEND", "software", 1);
        execv(executable.c_str(), pointers.data());
        _exit(127);
    }
    ::close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    std::string output;
    bool resource = false;
    bool timedOut = false;
    const auto started = std::chrono::steady_clock::now();
    int status = 0;
    bool reaped = false;
    while (!reaped) {
        const pid_t got = waitpid(pid, &status, WNOHANG);
        char buffer[1024];
        const ssize_t count = ::read(pipefd[0], buffer, sizeof buffer);
        if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
        if (got == pid) {
            reaped = true;
            break;
        }
        if (currentRssKb(pid) > kRssKillKb) {
            resource = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            reaped = true;
            break;
        }
        if (since(started) > static_cast<std::uint64_t>(spec.timeoutMs)) {
            timedOut = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            reaped = true;
            break;
        }
        pollfd ready {pipefd[0], POLLIN, 0};
        poll(&ready, 1, 50);
    }
    char buffer[1024];
    ssize_t count = 0;
    while ((count = ::read(pipefd[0], buffer, sizeof buffer)) > 0) output.append(buffer, static_cast<std::size_t>(count));
    ::close(pipefd[0]);
    std::vector<std::string> failures;
    removeScratch(scratch, &failures);
    if (!failures.empty()) *problem = failures.front();
    std::vector<Record> records;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("MEASURE\t", 0) != 0) continue;
        const auto fields = splitTab(line);
        if (fields.size() != 10) continue;
        Record record;
        record.kind = fields[1];
        record.name = fields[2];
        record.outcome = fields[3];
        record.bytes = std::strtoull(fields[4].c_str(), nullptr, 10);
        record.lines = std::strtoull(fields[5].c_str(), nullptr, 10);
        record.maxLine = std::strtoull(fields[6].c_str(), nullptr, 10);
        if (fields[7] != "-") record.elapsedMs = std::strtoull(fields[7].c_str(), nullptr, 10);
        record.rssKb = std::strtoull(fields[8].c_str(), nullptr, 10);
        record.detail = fields[9];
        record.metric = metricOf(record.name, spec.mode);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && record.outcome == "completed") record.outcome = "inconsistent";
        records.push_back(record);
    }
    if (resource || timedOut || records.empty()) {
        const char *outcome = resource ? "unsupported-resource" : timedOut ? "unsupported-timeout" : "crashed";
        const char *detail = resource ? "killed after resident set exceeded 1536 MiB"
            : timedOut ? "killed after the measurement ceiling"
            : "no measurement record";
        for (const auto &expected : recordNames(spec)) {
            bool present = false;
            for (const auto &record : records) if (record.name == expected) present = true;
            if (present) continue;
            Record record;
            record.kind = spec.kind;
            record.name = expected;
            record.metric = metricOf(expected, spec.mode);
            record.outcome = outcome;
            record.bytes = spec.bytes;
            record.lines = spec.lines;
            record.maxLine = std::string(spec.mode) == "long" ? spec.bytes : kWidth;
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
    if (item.metric == "key") return *mid > static_cast<std::uint64_t>(kKeyStallMs);
    if (item.metric == "read") return *mid > static_cast<std::uint64_t>(kGitStallMs);
    if (item.metric == "commit" || item.metric == "reload") return *mid > static_cast<std::uint64_t>(kRecoveryStallMs);
    return *mid > static_cast<std::uint64_t>(kLayoutStallMs);
}

std::vector<Aggregate> measure(const std::vector<Spec> &specs, std::string *problem)
{
    const std::string executable = selfPath();
    std::map<std::string, bool> halted;
    std::vector<Aggregate> results;
    auto findOrAdd = [&](const Record &record, const std::string &group) -> Aggregate & {
        for (auto &item : results) if (item.name == record.name) return item;
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
                record.metric = metricOf(expected, spec.mode);
                record.outcome = "not-attempted";
                record.lines = spec.lines;
                record.bytes = spec.bytes;
                record.maxLine = std::string(spec.mode) == "long" ? spec.bytes : kWidth;
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
            const auto records = runOnce(spec, executable, problem);
            for (const auto &record : records) {
                auto &item = findOrAdd(record, spec.group);
                if (!item.ranked || outcomeRank(record.outcome) < outcomeRank(item.outcome)) item.outcome = record.outcome;
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
    while ((count = ::read(pipefd[0], buffer, sizeof buffer)) > 0) output.append(buffer, static_cast<std::size_t>(count));
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

std::optional<std::uint64_t> qualifying(const std::vector<Aggregate> &results, const char *group, const char *metric,
                                        int budgetMs, SizeKind kind, bool capGit)
{
    std::optional<std::uint64_t> best;
    for (const auto &item : results) {
        if (item.group != group || item.metric != metric || item.outcome != "completed") continue;
        const auto mid = medianOf(item.elapsed);
        if (!mid || *mid > static_cast<std::uint64_t>(budgetMs)) continue;
        const std::uint64_t size = kind == SizeKind::Lines ? item.lines : kind == SizeKind::MaxLine ? item.maxLine : item.bytes;
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

void writeReports(const std::string &path, const std::vector<Aggregate> &results, const std::string &cleanup)
{
    std::ofstream markdown(path);
    utsname system {};
    uname(&system);
    const auto mount = capture({"/usr/bin/findmnt", "-T", "/var/tmp", "-n", "-o", "FSTYPE,SOURCE"});
    markdown << "# Workload measurements\n\n";
    markdown << "Build type " << BRANCH_TRUCE_MEASUREMENT_BUILD << ". Compiler " << __VERSION__
             << ". Qt " << qVersion() << ". Git " << capture({"/usr/bin/git", "--version"}) << ".\n\n";
    markdown << "Machine " << system.sysname << " " << system.release << " " << system.machine
             << ", " << cpuModel() << ", " << sysconf(_SC_NPROCESSORS_ONLN) << " processors, " << memTotal()
             << ". Scratch filesystem " << (mount.empty() ? "not reported" : mount) << ".\n\n";
    markdown << "Editor samples use QT_QPA_PLATFORM=offscreen and QT_QUICK_BACKEND=software. "
             << "They are not a Wayland or KDE session. Recovery and Git scratch live under /var/tmp "
             << "because /tmp is tmpfs on this host.\n\n";
    markdown << "## Decision rule\n\n";
    markdown << "A completed sample can enter the proposed envelope only when every repeat completed "
             << "and the median elapsed time is within the budget. The proposed size is the largest measured "
             << "point that qualifies. Sizes that were not attempted are not claimed. Timeouts, resource kills, "
             << "truncation, unacknowledged edits, explicit failures and fabricated success stay outside the envelope.\n\n";
    markdown << "Budgets are correspondence construction 1000 ms, correspondence edit 1000 ms, "
             << "editor load 1000 ms, editor keystroke 100 ms, Git read 2000 ms, and recovery acknowledgment 500 ms. "
             << "The Git figure cannot exceed the existing 8 MiB output guard.\n\n";
    markdown << "## Results\n\n";
    markdown << "| Name | Outcome | Bytes | Lines | Max line | Samples | Min ms | Median ms | Max ms | RSS KiB |\n";
    markdown << "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n";
    std::string tsvPath = path;
    const auto dot = tsvPath.rfind('.');
    if (dot == std::string::npos) tsvPath += ".tsv";
    else tsvPath.replace(dot, std::string::npos, ".tsv");
    std::ofstream table(tsvPath);
    table << "kind\tname\tmetric\tgroup\toutcome\tbytes\tlines\tmax_line\tsamples\tmin_ms\tmedian_ms\tmax_ms\trss_kb\tdetail\n";
    for (const auto &item : results) {
        const auto mid = medianOf(item.elapsed);
        const auto min = item.elapsed.empty() ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{*std::min_element(item.elapsed.begin(), item.elapsed.end())};
        const auto max = item.elapsed.empty() ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{*std::max_element(item.elapsed.begin(), item.elapsed.end())};
        const auto show = [](const std::optional<std::uint64_t> &value) {
            return value ? std::to_string(*value) : std::string("-");
        };
        markdown << "| " << item.name << " | " << item.outcome << " | " << item.bytes << " | " << item.lines
                 << " | " << item.maxLine << " | " << item.samples << " | " << show(min) << " | " << show(mid)
                 << " | " << show(max) << " | " << (item.rssKb == 0 ? std::string("-") : std::to_string(item.rssKb)) << " |\n";
        table << item.kind << '\t' << item.name << '\t' << item.metric << '\t' << item.group << '\t' << item.outcome << '\t'
              << item.bytes << '\t' << item.lines << '\t' << item.maxLine << '\t' << item.samples << '\t'
              << show(min) << '\t' << show(mid) << '\t' << show(max) << '\t' << item.rssKb << '\t' << sanitize(item.detail) << '\n';
    }
    const auto corrLines = qualifying(results, "corr-lines", "construct", kCorrespondenceBudgetMs, SizeKind::Lines, false);
    const auto corrEdit = qualifying(results, "corr-lines", "edit", kCorrespondenceBudgetMs, SizeKind::Lines, false);
    const auto editorLines = qualifying(results, "editor-lines", "load", kLoadBudgetMs, SizeKind::Lines, false);
    const auto editorKey = qualifying(results, "editor-lines", "key", kKeyBudgetMs, SizeKind::Lines, false);
    const auto longLoad = qualifying(results, "editor-long", "load", kLoadBudgetMs, SizeKind::MaxLine, false);
    const auto longKey = qualifying(results, "editor-long", "key", kKeyBudgetMs, SizeKind::MaxLine, false);
    const auto longConstruct = qualifying(results, "corr-long", "construct", kCorrespondenceBudgetMs, SizeKind::MaxLine, false);
    const auto longEdit = qualifying(results, "corr-long", "edit", kCorrespondenceBudgetMs, SizeKind::MaxLine, false);
    const auto gitRead = qualifying(results, "git-read", "read", kGitBudgetMs, SizeKind::Bytes, true);
    const auto recoveryCommit = qualifying(results, "recovery", "commit", kRecoveryBudgetMs, SizeKind::Bytes, false);
    const auto recoveryReload = qualifying(results, "recovery", "reload", kRecoveryBudgetMs, SizeKind::Bytes, false);
    const auto smallest = [](std::initializer_list<std::optional<std::uint64_t>> values) {
        std::optional<std::uint64_t> best;
        for (const auto &value : values) {
            if (!value) return std::optional<std::uint64_t>{};
            if (!best || *value < *best) best = value;
        }
        return best;
    };
    markdown << "\n## Proposed envelope for TRUCE-0029\n\n";
    markdown << "Component bounds before taking the tightest measured point: correspondence construction "
             << sizeOrNone(corrLines, "lines") << ", correspondence edit " << sizeOrNone(corrEdit, "lines")
             << ", editor load " << sizeOrNone(editorLines, "lines") << ", editor keystroke "
             << sizeOrNone(editorKey, "lines") << ", long-line editor load " << sizeOrNone(longLoad, "bytes")
             << ", long-line editor keystroke " << sizeOrNone(longKey, "bytes")
             << ", long-line correspondence construction " << sizeOrNone(longConstruct, "bytes")
             << ", long-line correspondence edit " << sizeOrNone(longEdit, "bytes")
             << ", Git read " << sizeOrNone(gitRead, "bytes") << ", recovery commit "
             << sizeOrNone(recoveryCommit, "bytes") << ", recovery reload "
             << sizeOrNone(recoveryReload, "bytes") << ".\n\n";
    markdown << "Representative files, three sources, width 96 bytes including the newline: up to "
             << sizeOrNone(smallest({corrLines, corrEdit, editorLines, editorKey}), "lines") << ".\n\n";
    markdown << "Adverse long lines, three panes or three sources: up to "
             << sizeOrNone(smallest({longLoad, longKey, longConstruct, longEdit}), "bytes on the longest line") << ".\n\n";
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
        if (item.outcome != "completed" || !mid) continue;
        int budget = -1;
        if (item.metric == "construct" || item.metric == "edit") budget = kCorrespondenceBudgetMs;
        else if (item.metric == "load") budget = kLoadBudgetMs;
        else if (item.metric == "key") budget = kKeyBudgetMs;
        else if (item.metric == "read") budget = kGitBudgetMs;
        else if (item.metric == "commit" || item.metric == "reload") budget = kRecoveryBudgetMs;
        if (budget >= 0 && *mid > static_cast<std::uint64_t>(budget))
            markdown << "- " << item.name << " median " << *mid << " ms, budget " << budget << " ms\n";
    }
    markdown << "\n";
    markdown << "Identical-buffer correspondence is a direct path and is excluded from the line-count proposal. "
             << "One failed or skipped probe means that part of the envelope is not established.\n\n";
    markdown << "## Outcomes outside the envelope\n\n";
    for (const auto &item : results) {
        if (item.outcome != "completed") markdown << "- " << item.name << " " << item.outcome << ". " << item.detail << "\n";
    }
    markdown << "\n## Limitations\n\n";
    markdown << "Each evidence size was repeated three times on one host. The median is not a population statistic. "
             << "Repeats can hit the page cache. The default developer build is Debug. These numbers come from the "
             << BRANCH_TRUCE_MEASUREMENT_BUILD << " binary. Editor latency does not include a physical keyboard, "
             << "input method, or KDE compositor. Correspondence cost is quadratic in line count. A long line is "
             << "interned once and is not the quadratic case. RSS is the process high-water mark and later work in "
             << "the same process can only raise it. Cleanup: " << cleanup << "\n";
}

struct Expectation {
    const char *name;
    const char *outcome;
    const char *detail = nullptr;
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
        for (const auto &item : results) if (item.name == expected.name) found = &item;
        if (found == nullptr || found->outcome != expected.outcome
            || (expected.detail != nullptr && found->detail.find(expected.detail) == std::string::npos)) {
            ++failures;
            std::cerr << "FAIL " << expected.name << " expected " << expected.outcome;
            if (expected.detail != nullptr) std::cerr << " detail " << expected.detail;
            std::cerr << " actual " << (found == nullptr ? "missing" : found->outcome + " " + found->detail) << '\n';
        } else {
            std::cout << "PASS " << expected.name << '\n';
        }
    }
    std::cout << "RESULT workload-measurements tests=" << tests << " assertions=" << assertions
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

struct OwnedSpec {
    Spec spec;
    std::string name;
};

std::vector<OwnedSpec> buildEvidence()
{
    std::vector<OwnedSpec> specs;
    auto add = [&](const char *group, const char *kind, std::string name, const char *mode, std::size_t lines,
                   std::size_t bytes, int timeoutMs, int repeats, bool escalating, bool startup) {
        OwnedSpec owned;
        owned.name = std::move(name);
        owned.spec = {group, kind, owned.name.c_str(), mode, lines, bytes, timeoutMs, repeats, escalating, startup};
        specs.push_back(std::move(owned));
    };
    for (std::size_t lines : {std::size_t{200}, std::size_t{1000}, std::size_t{2000}, std::size_t{4000}, std::size_t{8000}, std::size_t{16000}})
        add("corr-lines", "correspondence", "correspondence-lines-" + std::to_string(lines), "lines", lines, 0, lines >= 8000 ? 45000 : 20000, 3, true, false);
    for (std::size_t bytes : {std::size_t{4096}, std::size_t{65536}, std::size_t{1048576}, kGitGuard})
        add("corr-long", "correspondence", "correspondence-long-" + std::to_string(bytes), "long", 0, bytes, 20000, 3, true, false);
    add("corr-control", "correspondence", "correspondence-identical-4000", "identical", 4000, 0, 20000, 1, false, false);
    bool firstEditor = true;
    for (std::size_t lines : {std::size_t{200}, std::size_t{2000}, std::size_t{10000}, std::size_t{20000}}) {
        add("editor-lines", "editor", "editor-lines-" + std::to_string(lines), "lines", lines, 0, 30000, 3, true, firstEditor);
        firstEditor = false;
    }
    for (std::size_t bytes : {std::size_t{1024}, std::size_t{8192}, std::size_t{65536}, std::size_t{262144}, std::size_t{1048576}, std::size_t{4194304}})
        add("editor-long", "editor", "editor-long-" + std::to_string(bytes), "long", 0, bytes, 30000, 3, true, false);
    for (std::size_t bytes : {std::size_t{65536}, std::size_t{1048576}, std::size_t{4194304}, kGitGuard})
        add("git-read", "git", "git-read-" + std::to_string(bytes), "read", 0, bytes, 30000, 3, true, false);
    add("git-probe", "git", "git-read-over-limit", "read", 0, kGitGuard + 4096, 30000, 1, false, false);
    add("git-probe", "git", "git-input-over-limit", "input", 0, kGitGuard + 1, 20000, 1, false, false);
    for (std::size_t bytes : {std::size_t{4096}, std::size_t{65536}, std::size_t{1048576}, std::size_t{4194304}, kGitGuard})
        add("recovery", "recovery", "recovery-" + std::to_string(bytes), "commit", 0, bytes, 30000, 3, true, false);
    add("recovery-probe", "recovery", "recovery-bad-digest", "bad-digest", 0, 64, 20000, 1, false, false);
    add("recovery-probe", "recovery", "recovery-overload", "overload", 0, 1024ull * 1024ull, 20000, 1, false, false);
    for (auto &owned : specs) owned.spec.name = owned.name.c_str();
    return specs;
}

std::vector<OwnedSpec> buildGate()
{
    std::vector<OwnedSpec> specs;
    auto add = [&](const char *group, const char *kind, std::string name, const char *mode, std::size_t lines,
                   std::size_t bytes, int timeoutMs, bool startup) {
        OwnedSpec owned;
        owned.name = std::move(name);
        owned.spec = {group, kind, nullptr, mode, lines, bytes, timeoutMs, 1, false, startup};
        specs.push_back(std::move(owned));
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
    for (auto &owned : specs) owned.spec.name = owned.name.c_str();
    return specs;
}

std::vector<Spec> view(const std::vector<OwnedSpec> &owned)
{
    std::vector<Spec> specs;
    for (const auto &item : owned) specs.push_back(item.spec);
    return specs;
}

const std::vector<Expectation> kHonesty = {
    {"git-read-over-limit", "explicit-failure", "api=OutputLimit"},
    {"git-read-over-limit", "explicit-failure", "incomplete=1"},
    {"git-input-over-limit", "explicit-failure", "api=Rejected"},
    {"recovery-bad-digest", "explicit-failure", "acknowledged=0"},
    {"recovery-overload", "explicit-failure", "acknowledged=0"},
};

const std::vector<Expectation> kGate = {
    {"correspondence-lines-200-construct", "completed", nullptr},
    {"correspondence-lines-200-edit", "completed", nullptr},
    {"correspondence-long-65536-construct", "completed", nullptr},
    {"correspondence-long-65536-edit", "completed", nullptr},
    {"editor-startup", "completed", nullptr},
    {"editor-lines-200-load", "completed", nullptr},
    {"editor-lines-200-key", "completed", nullptr},
    {"editor-long-8192-load", "completed", nullptr},
    {"editor-long-8192-key", "completed", nullptr},
    {"git-read-65536", "completed", "api=Success"},
    {"recovery-65536-commit", "completed", "acknowledged=1"},
    {"recovery-65536-reload", "completed", nullptr},
};
}

int main(int argc, char **argv)
{
    gArgc = argc;
    gArgv = argv;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--worker") return workerMain(argc, argv);
    std::string profile = "gate";
    std::string report;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--profile" && i + 1 < argc) profile = argv[++i];
        else if (key == "--report" && i + 1 < argc) report = argv[++i];
        else if (key == "--help") {
            std::cout << "usage: workload-measurements --profile gate|evidence [--report PATH]\n";
            return 0;
        }
    }
    std::string problem;
    const auto owned = profile == "evidence" ? buildEvidence() : buildGate();
    const auto results = measure(view(owned), &problem);
    const std::string cleanup = problem.empty() ? "scratch directories removed" : problem;
    if (profile == "evidence") {
        if (report.empty()) report = "tests/fixtures/performance/evidence.md";
        writeReports(report, results, cleanup);
        std::cout << "report " << report << '\n';
    }
    std::vector<Expectation> expectations = profile == "evidence" ? kHonesty : kGate;
    if (profile != "evidence") expectations.insert(expectations.end(), kHonesty.begin(), kHonesty.end());
    for (const auto &item : results) {
        if (item.outcome == "fabricated-success") {
            expectations.push_back({item.name.c_str(), "completed", nullptr});
        }
    }
    return expect(results, expectations);
}
