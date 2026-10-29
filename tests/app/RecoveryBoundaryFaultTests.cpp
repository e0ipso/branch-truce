#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "recovery/RecoveryRestore.h"
#include "app/SessionModel.h"
#include "storage/DestinationWriter.h"
#include "TestHarness.h"
#include "fixtures/RecoveryJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

using namespace branch_truce;

extern "C" int __real_fsync(int descriptor);
extern "C" int __wrap_fsync(int descriptor)
{
    const char *env = std::getenv("TRUCE_CRASH_FSYNC");
    if (env == nullptr || env[0] == '\0') return __real_fsync(descriptor);
    const std::string mode(env);
    char path[4096];
    const auto link = "/proc/self/fd/" + std::to_string(descriptor);
    const auto size = ::readlink(link.c_str(), path, sizeof(path));
    if (size >= 0) {
        const std::string_view target(path, static_cast<std::size_t>(size));
        const bool journal = mode == "journal" || mode == "eio-journal";
        const bool matches = journal ? target.ends_with("/journal")
            : mode == "payload" && target.find("/payloads/") != std::string_view::npos;
        if (matches) {
            unsetenv("TRUCE_CRASH_FSYNC");
            if (mode == "eio-journal") {
                errno = EIO;
                return -1;
            }
            std::_Exit(42);
        }
    }
    return __real_fsync(descriptor);
}

namespace {
Draft baseDraft()
{
    return {"base line\n", {{"left", {DecisionValue::Accepted, 1}}}, 0};
}
Draft savedDraft()
{
    return {"saved checkpoint \xc3\xa9\n",
        {{"left", {DecisionValue::Accepted, 1}}, {"right", {DecisionValue::Ignored, {}}}}, 0};
}
Draft discardedDraft()
{
    return {"discarded unsaved \xf0\x9f\x98\x80\n",
        {{"left", {DecisionValue::Accepted, 1}}, {"right", {DecisionValue::Accepted, 2}}}, 0};
}

QString outcomeName(const std::optional<OperationOutcome> &outcome)
{
    if (!outcome) return QStringLiteral("none");
    switch (outcome->outcome) {
    case Outcome::Succeeded: return QStringLiteral("succeeded");
    case Outcome::Failed: return QStringLiteral("failed");
    case Outcome::Uncertain: return QStringLiteral("uncertain");
    }
    return QStringLiteral("none");
}

QJsonObject observation(const FileSession &file)
{
    QJsonArray decisions;
    for (const auto &entry : file.current().draft.decisions) {
        QJsonObject decision{{QStringLiteral("id"), QString::fromStdString(entry.first)},
            {QStringLiteral("value"), static_cast<int>(entry.second.value)}};
        if (entry.second.order) decision.insert(QStringLiteral("order"), QString::number(*entry.second.order));
        decisions.push_back(decision);
    }
    const auto facts = file.facts();
    return {{QStringLiteral("text"), QString::fromStdString(file.current().draft.text)},
        {QStringLiteral("decisions"), decisions},
        {QStringLiteral("revision"), QString::number(file.current().id)},
        {QStringLiteral("status"), QString::fromUtf8(statusText(facts.status))},
        {QStringLiteral("unsaved"), facts.unsaved},
        {QStringLiteral("resolved"), facts.resolved},
        {QStringLiteral("checkpoint"), file.checkpoint()
            ? QJsonValue(QString::fromStdString(file.checkpoint()->draft.text)) : QJsonValue()},
        {QStringLiteral("written"), file.written().has_value()},
        {QStringLiteral("staged"), file.staged().has_value()},
        {QStringLiteral("stageObject"), file.staged()
            ? QJsonValue(QString::fromStdString(file.staged()->objectId)) : QJsonValue()},
        {QStringLiteral("checkpointOutcome"), outcomeName(file.checkpointOutcome())},
        {QStringLiteral("writeOutcome"), outcomeName(file.writeOutcome())},
        {QStringLiteral("stageOutcome"), outcomeName(file.stageOutcome())}};
}

void emitObservation(const char *label, const FileSession &file)
{
    const auto line = QJsonDocument(observation(file)).toJson(QJsonDocument::Compact);
    std::cout << label << ' ';
    std::cout.write(line.constData(), static_cast<std::streamsize>(line.size()));
    std::cout << '\n';
    std::cout.flush();
}

template<typename ModelAction, typename FileAction>
[[noreturn]] void commitBoundary(SessionModel &model, const QString &crash, ModelAction modelAction, FileAction fileAction)
{
    auto predicted = *model.file("file");
    CHECK(fileAction(predicted) == Transition::Applied);
    emitObservation("BEFORE", *model.file("file"));
    emitObservation("EXPECTED", predicted);
    if (!crash.isEmpty()) qputenv("TRUCE_CRASH_FSYNC", crash.toUtf8());
    CHECK(modelAction(model) == Transition::Applied);
    qunsetenv("TRUCE_CRASH_FSYNC");
    std::_Exit(0);
}

void openBase(SessionModel &model)
{
    CHECK(model.appendFile(FileSession("file", "file.txt", baseDraft())));
}
void saveCheckpoint(SessionModel &model)
{
    openBase(model);
    CHECK(model.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
    CHECK(model.recordCheckpoint("file", model.file("file")->current().id, Outcome::Succeeded) == Transition::Applied);
}
void reviewSaved(SessionModel &model)
{
    saveCheckpoint(model);
    CHECK(model.beginMarkResolved("file", model.file("file")->current().id) == Transition::Applied);
}
void writeSaved(SessionModel &model)
{
    reviewSaved(model);
    CHECK(model.recordWrite("file", model.file("file")->current().id, Outcome::Succeeded) == Transition::Applied);
}

[[noreturn]] void runBoundary(const QString &name, const QString &directory, const QString &crash)
{
    CHECK(QDir::isAbsolutePath(directory));
    SessionModel model(directory);
    if (name == QLatin1String("edit")) {
        openBase(model);
        commitBoundary(model, crash,
            [](SessionModel &session) { return session.replaceDraft("file", 1, savedDraft()); },
            [](FileSession &file) { return file.replaceDraft(file.current().id, savedDraft()); });
    }
    if (name == QLatin1String("checkpoint")) {
        openBase(model);
        CHECK(model.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) { return session.recordCheckpoint("file", revision, Outcome::Succeeded); },
            [=](FileSession &file) { return file.recordCheckpoint(revision, Outcome::Succeeded); });
    }
    if (name == QLatin1String("discard")) {
        saveCheckpoint(model);
        CHECK(model.replaceDraft("file", model.file("file")->current().id, discardedDraft()) == Transition::Applied);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) { return session.discardToCheckpoint("file", revision); },
            [=](FileSession &file) { return file.discardToCheckpoint(file.current().id); });
    }
    if (name == QLatin1String("write")) {
        reviewSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) { return session.recordWrite("file", revision, Outcome::Succeeded); },
            [=](FileSession &file) { return file.recordWrite(revision, Outcome::Succeeded); });
    }
    if (name == QLatin1String("stage")) {
        writeSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) {
                return session.recordStage("file", revision, Outcome::Succeeded, "verified-object", "100644");
            },
            [=](FileSession &file) {
                return file.recordStage(revision, Outcome::Succeeded, "verified-object", "100644");
            });
    }
    if (name == QLatin1String("checkpoint-failed")) {
        openBase(model);
        CHECK(model.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) {
                return session.recordCheckpoint("file", revision, Outcome::Failed, "checkpoint sync failed");
            },
            [=](FileSession &file) { return file.recordCheckpoint(revision, Outcome::Failed, "checkpoint sync failed"); });
    }
    if (name == QLatin1String("write-failed")) {
        reviewSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) {
                return session.recordWrite("file", revision, Outcome::Failed, "repository write failed");
            },
            [=](FileSession &file) { return file.recordWrite(revision, Outcome::Failed, "repository write failed"); });
    }
    if (name == QLatin1String("write-uncertain")) {
        reviewSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) {
                return session.recordWrite("file", revision, Outcome::Uncertain, "destination changed during write");
            },
            [=](FileSession &file) {
                return file.recordWrite(revision, Outcome::Uncertain, "destination changed during write");
            });
    }
    if (name == QLatin1String("stage-failed")) {
        writeSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(model, crash,
            [=](SessionModel &session) {
                return session.recordStage("file", revision, Outcome::Failed, {}, {}, "index update failed");
            },
            [=](FileSession &file) { return file.recordStage(revision, Outcome::Failed, {}, {}, "index update failed"); });
    }
    throw std::runtime_error("unknown recovery boundary");
}

struct ChildResult {
    QProcess::ExitStatus status = QProcess::CrashExit;
    int code = -1;
    QByteArray output;
    QByteArray error;
};

ChildResult runChild(const QString &boundary, const QString &directory, const QString &crash)
{
    QProcess process;
    QStringList arguments{QStringLiteral("--boundary"), boundary, QStringLiteral("--dir"), directory};
    if (!crash.isEmpty()) arguments << QStringLiteral("--crash") << crash;
    process.start(QCoreApplication::applicationFilePath(), arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(5000);
        throw std::runtime_error("boundary child timed out: " + process.readAllStandardError().toStdString());
    }
    return {process.exitStatus(), process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
}

void requireChild(const ChildResult &child, int code)
{
    if (child.status != QProcess::NormalExit || child.code != code)
        throw std::runtime_error("child exit " + std::to_string(child.code) + " status " + std::to_string(static_cast<int>(child.status))
            + "\n" + child.error.toStdString());
}

QJsonObject labeled(const QByteArray &output, const QByteArray &label)
{
    for (const auto &line : output.split('\n')) {
        if (!line.startsWith(label + ' ')) continue;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line.mid(label.size() + 1), &error);
        CHECK(error.error == QJsonParseError::NoError && document.isObject());
        return document.object();
    }
    throw std::runtime_error("missing " + label.toStdString() + " in\n" + output.toStdString());
}

QJsonObject replayed(const QString &directory, bool *incomplete)
{
    const auto replay = replayRecovery(storage::RecoveryStore(directory));
    if (!replay.succeeded) throw std::runtime_error("recovery replay: " + replay.error.toStdString());
    CHECK(replay.files.size() == 1);
    if (incomplete != nullptr) *incomplete = replay.incompleteTail;
    return observation(replay.files.front());
}

void truncateFinalFrame(const QString &directory)
{
    const auto path = directory + QStringLiteral("/journal");
    QFile journal(path);
    CHECK(journal.open(QIODevice::ReadOnly));
    auto bytes = journal.readAll();
    CHECK(journal.error() == QFileDevice::NoError);
    journal.close();
    CHECK(bytes.endsWith('\n') && bytes.count('\n') >= 2);
    const auto prior = bytes.lastIndexOf('\n', bytes.size() - 2);
    CHECK(prior > 0);
    const auto frame = bytes.size() - (prior + 1);
    CHECK(frame > 4);
    bytes.truncate(prior + 1 + frame / 2);
    CHECK(!bytes.endsWith('\n'));
    CHECK(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(journal.write(bytes) == bytes.size());
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(file.write(bytes) == bytes.size());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

// Restores the process environment even when fixture construction throws.
class ScopedEnvironment {
public:
    ScopedEnvironment() = default;
    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;
    void apply(const QProcessEnvironment &environment)
    {
        for (const auto &name : environment.keys()) {
            const auto key = name.toLatin1();
            if (!previous.contains(key))
                previous.insert(key, qEnvironmentVariableIsSet(key.constData()) ? std::optional<QByteArray>(qgetenv(key.constData())) : std::nullopt);
            qputenv(key.constData(), environment.value(name).toUtf8());
        }
    }
    ~ScopedEnvironment()
    {
        for (auto entry = previous.cbegin(); entry != previous.cend(); ++entry) {
            if (entry.value()) qputenv(entry.key().constData(), *entry.value());
            else qunsetenv(entry.key().constData());
        }
    }
private:
    QMap<QByteArray, std::optional<QByteArray>> previous;
};

// One real text conflict on result.txt, opened through the application so
// Mark resolved runs the product publication and Git index path.
class IndexConflict {
public:
    IndexConflict()
    {
        CHECK(directory.isValid() && home.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
        environment.insert(QStringLiteral("HOME"), home.path());
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.path() + QStringLiteral("/config"));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.path() + QStringLiteral("/cache"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), home.path() + QStringLiteral("/data"));
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
        environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
        environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
        git({QStringLiteral("init"), QStringLiteral("--quiet"), QStringLiteral("--initial-branch=main")});
        git({QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Branch Truce Tests")});
        git({QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("tests@example.invalid")});
        git({QStringLiteral("config"), QStringLiteral("core.autocrlf"), QStringLiteral("false")});
        commit("base\n", QStringLiteral("base"));
        git({QStringLiteral("checkout"), QStringLiteral("--quiet"), QStringLiteral("-b"), QStringLiteral("side")});
        commit("side\n", QStringLiteral("side"));
        git({QStringLiteral("checkout"), QStringLiteral("--quiet"), QStringLiteral("main")});
        commit("main\n", QStringLiteral("main"));
        git({QStringLiteral("merge"), QStringLiteral("--no-edit"), QStringLiteral("side")}, 1);
        writeFile(path(QStringLiteral("result.txt")), manual);
        scoped.apply(environment);
        controller = std::make_unique<ApplicationController>(gitOnPath());
        open();
        CHECK(controller->selectFile(id));
        auto draft = file().current().draft;
        draft.text = reviewed.toStdString();
        draft.unresolvedMarkers = 0;
        CHECK(controller->files()->replaceDraft(id.toStdString(), file().current().id, draft) == Transition::Applied);
    }
    IndexConflict(const IndexConflict &) = delete;
    IndexConflict &operator=(const IndexConflict &) = delete;

    QString path(const QString &relative) const { return root + QLatin1Char('/') + relative; }
    QByteArray git(const QStringList &arguments, int expected = 0) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start(QStringLiteral("/usr/bin/git"), QStringList{QStringLiteral("--no-pager"), QStringLiteral("--literal-pathspecs")} + arguments);
        CHECK(process.waitForStarted(5000) && process.waitForFinished(15000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    QByteArray indexEntries() const
    {
        return git({QStringLiteral("ls-files"), QStringLiteral("--stage"), QStringLiteral("--"), QStringLiteral("result.txt")});
    }
    const FileSession &file() const
    {
        const auto *selected = controller->files()->file(id.toStdString());
        CHECK(selected != nullptr);
        return *selected;
    }
    // Starts the real publisher on the current revision and waits until it records an outcome.
    void markResolved()
    {
        CHECK(controller->markResolved(id, QString::number(static_cast<qulonglong>(file().current().id)), true));
        QElapsedTimer timer;
        timer.start();
        const auto settled = [&] {
            return !controller->resolutionBusy() && (file().stageOutcome()
                || (file().writeOutcome() && file().writeOutcome()->outcome != Outcome::Succeeded));
        };
        while (!settled() && timer.elapsed() < 25000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (!settled()) throw std::runtime_error("publisher did not settle: " + controller->commandMessage().toStdString());
    }

    QTemporaryDir directory;
    QTemporaryDir home;
    QString root;
    QProcessEnvironment environment;
    ScopedEnvironment scoped;
    std::unique_ptr<ApplicationController> controller; // Destroyed before the environment is restored.
    const QString id = QString::fromLatin1(QByteArray("result.txt").toHex());
    const QByteArray manual = "manual merge bytes\n";
    const QByteArray reviewed = "reviewed in app\n";

private:
    void commit(const QByteArray &bytes, const QString &message)
    {
        writeFile(path(QStringLiteral("result.txt")), bytes);
        git({QStringLiteral("add"), QStringLiteral("--"), QStringLiteral("result.txt")});
        git({QStringLiteral("commit"), QStringLiteral("--quiet"), QStringLiteral("-m"), message});
    }
    void open()
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(controller->loader(), &SessionLoader::launchChanged, &loop, [&] {
            if (!controller->loader()->opening()) loop.quit();
        });
        CHECK(controller->loader()->openRepository(root));
        timeout.start(15000);
        if (controller->loader()->opening()) loop.exec();
        if (controller->loader()->opening() || controller->loader()->startupState() != QLatin1String("ready"))
            throw std::runtime_error("controller startup: " + controller->loader()->detail().toStdString());
    }
};

RecoveryReplay replayedJournal(const QString &directory)
{
    auto replay = replayRecovery(storage::RecoveryStore(directory));
    if (!replay.succeeded) throw std::runtime_error("recovery replay: " + replay.error.toStdString());
    CHECK(!replay.incompleteTail && replay.files.size() == 1);
    return replay;
}

const RecoveryMutation *journalMutation(const RecoveryReplay &replay, const char *kind)
{
    const RecoveryMutation *found = nullptr;
    for (const auto &mutation : replay.mutations) {
        if (mutation.kind != QLatin1String(kind)) continue;
        CHECK(found == nullptr);
        found = &mutation;
    }
    return found;
}

template<typename Prepare, typename ModelAction, typename FileAction>
void retryBoundary(const char *name, Prepare prepare, ModelAction modelAction, FileAction fileAction)
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    SessionModel model(directory.path());
    prepare(model);
    auto predicted = *model.file("file");
    CHECK(fileAction(predicted) == Transition::Applied);
    const auto before = observation(*model.file("file"));
    const auto recordsBefore = storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
    qputenv("TRUCE_CRASH_FSYNC", "eio-journal");
    CHECK(modelAction(model) == Transition::Blocked);
    qunsetenv("TRUCE_CRASH_FSYNC");
    CHECK(model.hasPendingRecovery());
    CHECK(observation(*model.file("file")) == before);
    CHECK(model.pendingDraft("file") != nullptr);
    CHECK(model.pendingDraft("file")->text == predicted.current().draft.text);
    CHECK(model.pendingDraft("file")->decisions == predicted.current().draft.decisions);
    const auto landed = storage::RecoveryStore(model.recoveryDirectory()).read();
    CHECK(landed.succeeded && !landed.incompleteTail);
    CHECK(landed.transactions.size() == recordsBefore + 1);
    const auto request = landed.transactions.back().value(QStringLiteral("transactionId")).toString();
    CHECK(model.retryPendingRecovery(QStringLiteral("file")));
    CHECK(!model.hasPendingRecovery());
    CHECK(observation(*model.file("file")) == observation(predicted));
    const auto durable = storage::RecoveryStore(model.recoveryDirectory()).read();
    CHECK(durable.succeeded && durable.transactions.size() == recordsBefore + 1);
    CHECK(durable.transactions.back().value(QStringLiteral("transactionId")).toString() == request);
    int copies = 0;
    for (const auto &state : durable.transactions)
        if (state.value(QStringLiteral("transactionId")).toString() == request) ++copies;
    CHECK(copies == 1);
    CHECK(!model.retryPendingRecovery(QStringLiteral("file")));
    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == recordsBefore + 1);
    bool incomplete = true;
    CHECK(replayed(model.recoveryDirectory(), &incomplete) == observation(predicted));
    CHECK(!incomplete);
    static_cast<void>(name);
}
}

class RecoveryBoundaryFaultTests {
public:
    int run()
    {
        qunsetenv("TRUCE_CRASH_FSYNC");
        test_support::Suite suite("RecoveryBoundaryFaultTests");
        const QStringList durable{QStringLiteral("edit"), QStringLiteral("checkpoint"), QStringLiteral("discard"),
            QStringLiteral("write"), QStringLiteral("stage")};
        QStringList recorded = durable;
        recorded << QStringLiteral("checkpoint-failed") << QStringLiteral("write-failed")
                 << QStringLiteral("write-uncertain") << QStringLiteral("stage-failed");
        for (const auto &boundary : recorded) {
            suite.run("TRUCE-0034 acknowledged " + boundary.toStdString() + " survives process exit with its text decisions and status",
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    const auto child = runChild(boundary, directory.path(), {});
                    requireChild(child, 0);
                    const auto expected = labeled(child.output, "EXPECTED");
                    bool incomplete = true;
                    CHECK(replayed(directory.path(), &incomplete) == expected);
                    CHECK(!incomplete);
                    if (boundary == QLatin1String("discard")) {
                        const auto before = labeled(child.output, "BEFORE");
                        CHECK(expected.value(QStringLiteral("text")) == QString::fromStdString(savedDraft().text));
                        CHECK(before.value(QStringLiteral("text")) == QString::fromStdString(discardedDraft().text));
                        CHECK(expected.value(QStringLiteral("text")) != before.value(QStringLiteral("text")));
                        CHECK(expected.value(QStringLiteral("checkpoint")) == expected.value(QStringLiteral("text")));
                    }
                });
            suite.run("TRUCE-0034 termination during " + boundary.toStdString() + " leaves the prior text decisions and status",
                [=] {
                    QTemporaryDir directory;
                    CHECK(directory.isValid());
                    const auto child = runChild(boundary, directory.path(), {});
                    requireChild(child, 0);
                    const auto before = labeled(child.output, "BEFORE");
                    const auto expected = labeled(child.output, "EXPECTED");
                    truncateFinalFrame(directory.path());
                    bool incomplete = false;
                    const auto restored = replayed(directory.path(), &incomplete);
                    CHECK(incomplete);
                    CHECK(restored == before);
                    CHECK(restored.value(QStringLiteral("text")) != expected.value(QStringLiteral("text"))
                        || restored.value(QStringLiteral("status")) != expected.value(QStringLiteral("status"))
                        || restored.value(QStringLiteral("stageOutcome")) != expected.value(QStringLiteral("stageOutcome"))
                        || restored.value(QStringLiteral("writeOutcome")) != expected.value(QStringLiteral("writeOutcome"))
                        || restored.value(QStringLiteral("checkpointOutcome")) != expected.value(QStringLiteral("checkpointOutcome")));
                });
        }
        for (const auto &boundary : durable) {
            suite.run("TRUCE-0034 journal sync death during " + boundary.toStdString() + " keeps the completed record", [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                const auto child = runChild(boundary, directory.path(), QStringLiteral("journal"));
                requireChild(child, 42);
                bool incomplete = true;
                CHECK(replayed(directory.path(), &incomplete) == labeled(child.output, "EXPECTED"));
                CHECK(!incomplete);
            });
        }
        suite.run("TRUCE-0034 payload sync death during edit keeps only the acknowledged draft", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto child = runChild(QStringLiteral("edit"), directory.path(), QStringLiteral("payload"));
            requireChild(child, 42);
            bool incomplete = true;
            const auto restored = replayed(directory.path(), &incomplete);
            CHECK(!incomplete);
            CHECK(restored == labeled(child.output, "BEFORE"));
            CHECK(restored.value(QStringLiteral("text")) != labeled(child.output, "EXPECTED").value(QStringLiteral("text")));
        });
        suite.run("TRUCE-0034 recovery-write retry keeps one edit and its decisions", [] {
            retryBoundary("edit", openBase,
                [](SessionModel &session) { return session.replaceDraft("file", 1, savedDraft()); },
                [](FileSession &file) { return file.replaceDraft(file.current().id, savedDraft()); });
        });
        suite.run("TRUCE-0034 checkpoint sync failure retains the draft and retries one manual save", [] {
            retryBoundary("checkpoint", [](SessionModel &session) {
                openBase(session);
                CHECK(session.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
            }, [](SessionModel &session) {
                return session.recordCheckpoint("file", session.file("file")->current().id, Outcome::Succeeded);
            }, [](FileSession &file) {
                return file.recordCheckpoint(file.current().id, Outcome::Succeeded);
            });
        });
        suite.run("TRUCE-0034 publication sync failure retains the reviewed draft and retries one write", [] {
            retryBoundary("write", reviewSaved, [](SessionModel &session) {
                return session.recordWrite("file", session.file("file")->current().id, Outcome::Succeeded);
            }, [](FileSession &file) {
                return file.recordWrite(file.current().id, Outcome::Succeeded);
            });
        });
        // A recovery-journal fsync failure while recording a stage; the Git index is not involved.
        suite.run("TRUCE-0034 stage outcome journal sync failure retains the written draft and retries one stage", [] {
            retryBoundary("stage", writeSaved, [](SessionModel &session) {
                return session.recordStage("file", session.file("file")->current().id, Outcome::Succeeded,
                    "verified-object", "100644");
            }, [](FileSession &file) {
                return file.recordStage(file.current().id, Outcome::Succeeded, "verified-object", "100644");
            });
        });
        suite.run("TRUCE-0034 a corrupt record is not restored as success", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            requireChild(runChild(QStringLiteral("edit"), directory.path(), {}), 0);
            const auto journalPath = directory.path() + QStringLiteral("/journal");
            auto corrupt = readFile(journalPath);
            const auto prior = corrupt.lastIndexOf('\n', corrupt.size() - 2);
            CHECK(prior > 0 && corrupt.size() > prior + 3);
            corrupt[prior + 2] = corrupt.at(prior + 2) == 'A' ? 'B' : 'A';
            writeFile(journalPath, corrupt);
            const auto replay = replayRecovery(storage::RecoveryStore(directory.path()));
            CHECK(!replay.succeeded && replay.files.empty() && replay.mutations.empty());
            CHECK(replay.error.contains(QStringLiteral("integrity")));
        });
        suite.run("TRUCE-0034 an unsupported schema is not restored as success", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            requireChild(runChild(QStringLiteral("edit"), directory.path(), {}), 0);
            test_support::rewriteLastRecoveryState(directory.path(), [](QJsonObject &state) { state.insert(QStringLiteral("schema"), 99); });
            const auto replay = replayRecovery(storage::RecoveryStore(directory.path()));
            CHECK(!replay.succeeded && replay.files.empty() && replay.mutations.empty());
            CHECK(replay.error.contains(QStringLiteral("schema")));
        });
        suite.run("TRUCE-0034 an unsupported journal header is not restored as success", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            requireChild(runChild(QStringLiteral("edit"), directory.path(), {}), 0);
            const auto journalPath = directory.path() + QStringLiteral("/journal");
            auto bytes = readFile(journalPath);
            CHECK(bytes.startsWith("Branch Truce recovery journal v1\n"));
            bytes.replace(0, sizeof("Branch Truce recovery journal v1\n") - 1, "Branch Truce recovery journal v9\n");
            writeFile(journalPath, bytes);
            const auto replay = replayRecovery(storage::RecoveryStore(directory.path()));
            CHECK(!replay.succeeded && replay.files.empty());
            CHECK(replay.error.contains(QStringLiteral("Unknown or corrupt")));
        });
        suite.run("TRUCE-0034 a mismatched file identity is not restored as success", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            requireChild(runChild(QStringLiteral("edit"), directory.path(), {}), 0);
            test_support::rewriteLastRecoveryState(directory.path(), [](QJsonObject &state) {
                state.insert(QStringLiteral("fileId"), state.value(QStringLiteral("path")));
            });
            const auto replay = replayRecovery(storage::RecoveryStore(directory.path()));
            CHECK(!replay.succeeded && replay.files.empty() && replay.mutations.empty());
            CHECK(replay.error.contains(QStringLiteral("matching open file")));
        });
        suite.run("TRUCE-0034 a stale destination rejects publication and keeps the acknowledged draft", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto root = QFileInfo(directory.path()).canonicalFilePath();
            const QByteArray original = "original manual result\n";
            const QByteArray external = "external replacement\n";
            const auto draft = savedDraft();
            writeFile(root + QStringLiteral("/result.txt"), original);
            CHECK(::chmod(QFile::encodeName(root + QStringLiteral("/result.txt")).constData(), 0644) == 0);
            const auto captured = storage::captureDestination(root, "result.txt");
            CHECK(captured.destination && captured.failure == storage::DestinationFailure::None);
            writeFile(root + QStringLiteral("/result.txt"), external);
            SessionModel model(root + QStringLiteral("/recovery"));
            CHECK(model.appendFile(FileSession("file", "result.txt", draft)));
            int admissions = 0;
            const auto published = storage::publishDestination(*captured.destination,
                QByteArray::fromStdString(draft.text), "100644", [&] { ++admissions; return true; });
            CHECK(admissions == 0);
            CHECK(published.state == storage::PublicationState::Unchanged);
            CHECK(published.failure == storage::DestinationFailure::ContentChanged);
            CHECK(readFile(root + QStringLiteral("/result.txt")) == external);
            CHECK(model.file("file")->current().draft == draft);
            CHECK(!model.file("file")->written() && !model.file("file")->staged());
            bool incomplete = true;
            const auto restored = replayed(model.recoveryDirectory(), &incomplete);
            CHECK(!incomplete);
            CHECK(restored.value(QStringLiteral("text")).toString().toStdString() == draft.text);
            CHECK(restored.value(QStringLiteral("status")).toString() != QLatin1String("Written and resolved"));
            CHECK(!restored.value(QStringLiteral("written")).toBool());
        });
        // Git index faults run through the real publisher; TRUCE_CRASH_FSYNC stays unset,
        // so the recovery journal syncs normally and records what the product observed.
        suite.run("TRUCE-0068 a replaced Git index entry stops publication and replay shows the recorded write failure", [] {
            IndexConflict conflict;
            const auto staleFile = conflict.home.path() + QStringLiteral("/stale-index-bytes");
            writeFile(staleFile, "stale index bytes\n");
            const auto stale = conflict.git({QStringLiteral("hash-object"), QStringLiteral("-w"),
                QStringLiteral("--no-filters"), staleFile}).trimmed();
            CHECK(stale.size() == 40);
            conflict.git({QStringLiteral("update-index"), QStringLiteral("--cacheinfo"),
                QStringLiteral("100644,") + QString::fromLatin1(stale) + QStringLiteral(",result.txt")});
            const auto replacedIndex = conflict.indexEntries();
            CHECK(replacedIndex == "100644 " + stale + " 0\tresult.txt\n");
            const auto revision = conflict.file().current().id;
            conflict.markResolved();
            const auto &file = conflict.file();
            CHECK(file.writeOutcome() && file.writeOutcome()->revision == revision
                && file.writeOutcome()->outcome == Outcome::Failed);
            CHECK(QString::fromStdString(file.writeOutcome()->detail).contains(QLatin1String("index no longer matches this conflict")));
            CHECK(!file.stageOutcome() && !file.written() && !file.staged() && !file.facts().resolved);
            CHECK(file.facts().status == FileStatus::WriteFailed);
            CHECK(file.current().draft.text == conflict.reviewed.toStdString() && file.checkpoint());
            CHECK(readFile(conflict.path(QStringLiteral("result.txt"))) == conflict.manual);
            CHECK(conflict.indexEntries() == replacedIndex);
            CHECK(!conflict.controller->files()->hasPendingRecovery());
            const auto replay = replayedJournal(conflict.controller->files()->recoveryDirectory());
            CHECK(replay.mutations.empty());
            const auto &restored = replay.files.front();
            CHECK(observation(restored) == observation(file));
            CHECK(observation(restored).value(QStringLiteral("writeOutcome")).toString() == QLatin1String("failed"));
            CHECK(restored.writeOutcome() && restored.writeOutcome()->detail == file.writeOutcome()->detail);
        });
        suite.run("TRUCE-0068 a locked Git index fails staging after the write and replay shows the recorded stage failure", [] {
            IndexConflict conflict;
            const auto conflicted = conflict.indexEntries();
            CHECK(conflicted.contains(" 1\tresult.txt\n") && conflicted.contains(" 2\tresult.txt\n")
                && conflicted.contains(" 3\tresult.txt\n"));
            // Git refuses every index update while another writer holds index.lock.
            const auto lock = conflict.path(QStringLiteral(".git/index.lock"));
            writeFile(lock, {});
            const auto revision = conflict.file().current().id;
            conflict.markResolved();
            const auto &file = conflict.file();
            CHECK(file.writeOutcome() && file.writeOutcome()->revision == revision
                && file.writeOutcome()->outcome == Outcome::Succeeded);
            CHECK(file.stageOutcome() && file.stageOutcome()->revision == revision
                && file.stageOutcome()->outcome == Outcome::Failed);
            CHECK(QString::fromStdString(file.stageOutcome()->detail).contains(QLatin1String("Git exited with code")));
            CHECK(file.written() && !file.staged() && !file.facts().resolved);
            CHECK(file.facts().status == FileStatus::StageFailed);
            CHECK(readFile(conflict.path(QStringLiteral("result.txt"))) == conflict.reviewed);
            CHECK(conflict.indexEntries() == conflicted);
            CHECK(QFile::exists(lock));
            CHECK(!conflict.controller->files()->hasPendingRecovery());
            CHECK(QFile::remove(lock));
            const auto replay = replayedJournal(conflict.controller->files()->recoveryDirectory());
            const auto *publication = journalMutation(replay, "publication");
            const auto *staging = journalMutation(replay, "staging");
            CHECK(publication && publication->observation == QLatin1String("changed") && publication->completed);
            CHECK(staging && staging->observation == QLatin1String("unchanged") && !staging->completed);
            const auto &restored = replay.files.front();
            CHECK(observation(restored) == observation(file));
            CHECK(observation(restored).value(QStringLiteral("stageOutcome")).toString() == QLatin1String("failed"));
            CHECK(restored.stageOutcome() && restored.stageOutcome()->detail == file.stageOutcome()->detail);
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() >= 5 && arguments.at(1) == QLatin1String("--boundary")) {
        QString boundary, directory, crash;
        for (int index = 1; index + 1 < arguments.size(); ++index) {
            if (arguments.at(index) == QLatin1String("--boundary")) boundary = arguments.at(++index);
            else if (arguments.at(index) == QLatin1String("--dir")) directory = arguments.at(++index);
            else if (arguments.at(index) == QLatin1String("--crash")) crash = arguments.at(++index);
        }
        try {
            runBoundary(boundary, directory, crash);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return RecoveryBoundaryFaultTests{}.run();
}
