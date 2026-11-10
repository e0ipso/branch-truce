#include "TestHarness.h"
#include "TestSession.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/EngineText.h"
#include "app/SessionModel.h"
#include "app/Translations.h"
#include "fixtures/AppSession.h"
#include "fixtures/ConflictedRepository.h"
#include "fixtures/Files.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/ScopedEnvironment.h"
#include "fixtures/SyncFaults.h"
#include "mergetool/MergetoolProtocol.h"
#include "recovery/RecoveryRestore.h"
#include "storage/DestinationWriter.h"

#include <QCoreApplication>
#include <QDir>
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

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

using namespace branch_truce;

namespace {
// Ends the boundary child the moment a matching recovery sync starts, as a power cut or a kill
// would, once the action under test arms it. "journal" matches the journal, "payload" a payload.
class CrashingFiles final : public storage::FileSystem {
public:
    explicit CrashingFiles(QString mode) : mode_(std::move(mode)) {}
    void arm() { armed_ = true; }
    void disarm() { armed_ = false; }
    int sync(int descriptor) const override
    {
        if (armed_ && !mode_.isEmpty()) {
            char path[4096];
            const auto link = "/proc/self/fd/" + std::to_string(descriptor);
            const auto size = ::readlink(link.c_str(), path, sizeof(path));
            if (size >= 0) {
                const std::string_view target(path, static_cast<std::size_t>(size));
                const bool matches = mode_ == QLatin1String("journal")
                    ? target.ends_with("/journal")
                    : mode_ == QLatin1String("payload") && target.find("/payloads/") != std::string_view::npos;
                if (matches) std::_Exit(42);
            }
        }
        return FileSystem::sync(descriptor);
    }

private:
    const QString mode_;
    std::atomic_bool armed_ = false;
};
}

namespace {
Draft baseDraft() { return {"base line\n", {{"left", {DecisionValue::Accepted, 1}}}, 0}; }
Draft savedDraft()
{
    return {"saved checkpoint \xc3\xa9\n",
            {{"left", {DecisionValue::Accepted, 1}}, {"right", {DecisionValue::Ignored, {}}}},
            0};
}
Draft discardedDraft()
{
    return {"discarded unsaved \xf0\x9f\x98\x80\n",
            {{"left", {DecisionValue::Accepted, 1}}, {"right", {DecisionValue::Accepted, 2}}},
            0};
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
            {QStringLiteral("status"), engine_text::status(facts.status)},
            {QStringLiteral("unsaved"), facts.unsaved},
            {QStringLiteral("resolved"), facts.resolved},
            {QStringLiteral("checkpoint"),
             file.checkpoint() ? QJsonValue(QString::fromStdString(file.checkpoint()->draft.text)) : QJsonValue()},
            {QStringLiteral("written"), file.written().has_value()},
            {QStringLiteral("staged"), file.staged().has_value()},
            {QStringLiteral("stageObject"),
             file.staged() ? QJsonValue(QString::fromStdString(file.staged()->objectId)) : QJsonValue()},
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

template <typename ModelAction, typename FileAction>
[[noreturn]] void commitBoundary(test_support::TestSession &model, CrashingFiles &files, ModelAction modelAction,
                                 FileAction fileAction)
{
    auto predicted = *model.file("file");
    CHECK(fileAction(predicted) == Transition::Applied);
    emitObservation("BEFORE", *model.file("file"));
    emitObservation("EXPECTED", predicted);
    files.arm();
    CHECK(modelAction(model) == Transition::Applied);
    files.disarm();
    std::_Exit(0);
}

void openBase(test_support::TestSession &model)
{
    CHECK(model.appendFile(FileSession("file", "file.txt", baseDraft())));
}
void saveCheckpoint(test_support::TestSession &model)
{
    openBase(model);
    CHECK(model.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
    CHECK(model.recordCheckpoint("file", model.file("file")->current().id, Outcome::Succeeded) == Transition::Applied);
}
void reviewSaved(test_support::TestSession &model)
{
    saveCheckpoint(model);
    CHECK(model.beginMarkResolved("file", model.file("file")->current().id) == Transition::Applied);
}
void writeSaved(test_support::TestSession &model)
{
    reviewSaved(model);
    CHECK(model.recordWrite("file", model.file("file")->current().id, Outcome::Succeeded) == Transition::Applied);
}

[[noreturn]] void runBoundary(const QString &name, const QString &directory, const QString &crash)
{
    CHECK(QDir::isAbsolutePath(directory));
    const auto files = std::make_shared<CrashingFiles>(crash);
    test_support::TestSession model(directory, files);
    if (name == QLatin1String("edit")) {
        openBase(model);
        commitBoundary(
            model, *files,
            [](test_support::TestSession &session) { return session.replaceDraft("file", 1, savedDraft()); },
            [](FileSession &file) { return file.replaceDraft(file.current().id, savedDraft()); });
    }
    if (name == QLatin1String("checkpoint")) {
        openBase(model);
        CHECK(model.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
                return session.recordCheckpoint("file", revision, Outcome::Succeeded);
            },
            [=](FileSession &file) { return file.recordCheckpoint(revision, Outcome::Succeeded); });
    }
    if (name == QLatin1String("discard")) {
        saveCheckpoint(model);
        CHECK(model.replaceDraft("file", model.file("file")->current().id, discardedDraft()) == Transition::Applied);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) { return session.discardToCheckpoint("file", revision); },
            [=](FileSession &file) { return file.discardToCheckpoint(file.current().id); });
    }
    if (name == QLatin1String("write")) {
        reviewSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
                return session.recordWrite("file", revision, Outcome::Succeeded);
            },
            [=](FileSession &file) { return file.recordWrite(revision, Outcome::Succeeded); });
    }
    if (name == QLatin1String("stage")) {
        writeSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
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
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
                return session.recordCheckpoint("file", revision, Outcome::Failed, "checkpoint sync failed");
            },
            [=](FileSession &file) {
                return file.recordCheckpoint(revision, Outcome::Failed, "checkpoint sync failed");
            });
    }
    if (name == QLatin1String("write-failed")) {
        reviewSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
                return session.recordWrite("file", revision, Outcome::Failed, "repository write failed");
            },
            [=](FileSession &file) { return file.recordWrite(revision, Outcome::Failed, "repository write failed"); });
    }
    if (name == QLatin1String("write-uncertain")) {
        reviewSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
                return session.recordWrite("file", revision, Outcome::Uncertain, "destination changed during write");
            },
            [=](FileSession &file) {
                return file.recordWrite(revision, Outcome::Uncertain, "destination changed during write");
            });
    }
    if (name == QLatin1String("stage-failed")) {
        writeSaved(model);
        const auto revision = model.file("file")->current().id;
        commitBoundary(
            model, *files,
            [=](test_support::TestSession &session) {
                return session.recordStage("file", revision, Outcome::Failed, {}, {}, "index update failed");
            },
            [=](FileSession &file) {
                return file.recordStage(revision, Outcome::Failed, {}, {}, "index update failed");
            });
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
        throw std::runtime_error("child exit " + std::to_string(child.code) + " status "
                                 + std::to_string(static_cast<int>(child.status)) + "\n" + child.error.toStdString());
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

// How the last frame of a journal can end after a crash or power loss.
enum class TornTail { Half, NoNewline, Zeroed };

void tearFinalFrame(const QString &directory, TornTail tail)
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
    switch (tail) {
    case TornTail::Half: bytes.truncate(prior + 1 + frame / 2); break;
    case TornTail::NoNewline: bytes.chop(1); break;
    case TornTail::Zeroed: bytes = bytes.left(prior + 1) + QByteArray(frame, '\0'); break;
    }
    CHECK(!bytes.endsWith('\n'));
    CHECK(journal.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(journal.write(bytes) == bytes.size());
}

using test_support::readFile;
using test_support::writeFile;

// One real text conflict on result.txt, opened through the application so
// Mark resolved runs the product publication and Git index path.
class IndexConflict : public test_support::ConflictedRepository {
public:
    IndexConflict()
    {
        commit({{QStringLiteral("result.txt"), "base\n"}}, QStringLiteral("base"));
        conflict({{QStringLiteral("result.txt"), "main\n"}}, {{QStringLiteral("result.txt"), "side\n"}});
        writeFile(path(QStringLiteral("result.txt")), manual);
        // Restores the process environment when it goes, even if the rest of construction throws.
        scoped.emplace(environment);
        controller = std::make_unique<Application>(Application::Config{gitOnPath(), newRecoveryDirectory(), {}});
        test_support::openRepository(*controller, root);
        if (controller->loader()->startupState() != QLatin1String("ready"))
            throw std::runtime_error("controller startup: " + controller->loader()->detail().toStdString());
        CHECK(controller->selection()->select(id));
        auto draft = file().current().draft;
        draft.text = reviewed.toStdString();
        draft.unresolvedMarkers = 0;
        CHECK(controller->store()->replaceDraft(id.toStdString(), file().current().id, draft) == Transition::Applied);
    }

    QByteArray indexEntries() const
    {
        return git({QStringLiteral("ls-files"), QStringLiteral("--stage"), QStringLiteral("--"),
                    QStringLiteral("result.txt")});
    }
    const FileSession &file() const
    {
        const auto *selected = controller->store()->file(id.toStdString());
        CHECK(selected != nullptr);
        return *selected;
    }
    // Starts the real publisher on the current revision and waits until it records an outcome.
    void markResolved()
    {
        CHECK(controller->commands()->markResolved(id, QString::number(static_cast<qulonglong>(file().current().id)),
                                                   true));
        test_support::waitUntil(
            [&] {
                return !controller->commands()->resolutionBusy()
                    && (file().stageOutcome()
                        || (file().writeOutcome() && file().writeOutcome()->outcome != Outcome::Succeeded));
            },
            [&] { return "publisher did not settle: " + controller->feedback()->message().toStdString(); });
    }

    std::optional<test_support::ScopedEnvironment> scoped;
    std::unique_ptr<Application> controller; // Destroyed before the environment is restored.
    const QString id = QString::fromLatin1(QByteArray("result.txt").toHex());
    const QByteArray manual = "manual merge bytes\n";
    const QByteArray reviewed = "reviewed in app\n";
};

RecoveryReplay replayedJournal(const QString &directory)
{
    auto replay = replayRecovery(storage::RecoveryStore(directory));
    if (!replay.succeeded) throw std::runtime_error("recovery replay: " + replay.error.toStdString());
    CHECK(!replay.incompleteTail && replay.files.size() == 1);
    return replay;
}

const RecoveryMutation *journalMutation(const RecoveryReplay &replay, MutationKind kind)
{
    const RecoveryMutation *found = nullptr;
    for (const auto &mutation : replay.mutations) {
        if (mutation.kind != kind) continue;
        CHECK(found == nullptr);
        found = &mutation;
    }
    return found;
}

template <typename Prepare, typename ModelAction, typename FileAction>
void retryBoundary(Prepare prepare, ModelAction modelAction, FileAction fileAction)
{
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto files = std::make_shared<test_support::SyncFaults>();
    test_support::TestSession model(directory.path(), files);
    prepare(model);
    auto predicted = *model.file("file");
    CHECK(fileAction(predicted) == Transition::Applied);
    const auto before = observation(*model.file("file"));
    const auto recordsBefore = storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size();
    {
        // Disarmed however the check below leaves the scope.
        test_support::ArmedFault fault(*files, test_support::SyncFaults::Point::JournalSync);
        CHECK(modelAction(model) == Transition::Blocked);
        CHECK_EQ(files->failures.load(), 1);
    }
    CHECK(model.hasPendingRecovery());
    CHECK(observation(*model.file("file")) == before);
    CHECK(model.pendingDraft("file") != nullptr);
    CHECK(model.pendingDraft("file")->text == predicted.current().draft.text);
    CHECK(model.pendingDraft("file")->decisions == predicted.current().draft.decisions);
    const auto landed = storage::RecoveryStore(model.recoveryDirectory()).read();
    CHECK(landed.succeeded && !landed.incompleteTail);
    CHECK(landed.transactions.size() == recordsBefore + 1);
    const auto request = landed.transactions.back().id;
    CHECK(model.retryPendingRecovery(QStringLiteral("file")));
    CHECK(!model.hasPendingRecovery());
    CHECK(observation(*model.file("file")) == observation(predicted));
    const auto durable = storage::RecoveryStore(model.recoveryDirectory()).read();
    CHECK(durable.succeeded && durable.transactions.size() == recordsBefore + 1);
    CHECK(durable.transactions.back().id == request);
    int copies = 0;
    for (const auto &stored : durable.transactions)
        if (stored.id == request) ++copies;
    CHECK(copies == 1);
    CHECK(!model.retryPendingRecovery(QStringLiteral("file")));
    CHECK(storage::RecoveryStore(model.recoveryDirectory()).read().transactions.size() == recordsBefore + 1);
    bool incomplete = true;
    CHECK(replayed(model.recoveryDirectory(), &incomplete) == observation(predicted));
    CHECK(!incomplete);
}
}

class RecoveryBoundaryFaultTests {
public:
    int run()
    {
        test_support::Suite suite("RecoveryBoundaryFaultTests");
        suite.run("controller fixtures contain and remove their session sockets and restore the runtime environment",
                  [] {
                      const auto previous = qgetenv("XDG_RUNTIME_DIR");
                      const auto wasSet = qEnvironmentVariableIsSet("XDG_RUNTIME_DIR");
                      QString runtime;
                      {
                          IndexConflict conflict;
                          runtime = conflict.environment.value(QStringLiteral("XDG_RUNTIME_DIR"));
                          CHECK(QDir::isAbsolutePath(runtime));
                          CHECK(qEnvironmentVariable("XDG_RUNTIME_DIR") == runtime);
                          CHECK(mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR"))
                                == runtime + QStringLiteral("/branch-truce"));
                          CHECK(QFileInfo::exists(mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR"))));
                      }
                      CHECK(qEnvironmentVariableIsSet("XDG_RUNTIME_DIR") == wasSet);
                      CHECK(qgetenv("XDG_RUNTIME_DIR") == previous);
                      CHECK(!QFileInfo::exists(runtime));
                  });
        const QStringList durable{QStringLiteral("edit"), QStringLiteral("checkpoint"), QStringLiteral("discard"),
                                  QStringLiteral("write"), QStringLiteral("stage")};
        QStringList recorded = durable;
        recorded << QStringLiteral("checkpoint-failed") << QStringLiteral("write-failed")
                 << QStringLiteral("write-uncertain") << QStringLiteral("stage-failed");
        for (const auto &boundary : recorded) {
            suite.run(
                "acknowledged " + boundary.toStdString() + " survives process exit with its text decisions and status",
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
            for (const auto &[tail, shape] :
                 {std::pair{TornTail::Half, "half"}, std::pair{TornTail::NoNewline, "unterminated"},
                  std::pair{TornTail::Zeroed, "zero-filled"}}) {
                suite.run("a " + std::string(shape) + " final frame during " + boundary.toStdString()
                              + " leaves the prior text decisions and status",
                          [=] {
                              QTemporaryDir directory;
                              CHECK(directory.isValid());
                              const auto child = runChild(boundary, directory.path(), {});
                              requireChild(child, 0);
                              const auto before = labeled(child.output, "BEFORE");
                              const auto expected = labeled(child.output, "EXPECTED");
                              tearFinalFrame(directory.path(), tail);
                              bool incomplete = false;
                              const auto restored = replayed(directory.path(), &incomplete);
                              CHECK(incomplete);
                              CHECK(restored == before);
                              CHECK(restored.value(QStringLiteral("text")) != expected.value(QStringLiteral("text"))
                                    || restored.value(QStringLiteral("status"))
                                        != expected.value(QStringLiteral("status"))
                                    || restored.value(QStringLiteral("stageOutcome"))
                                        != expected.value(QStringLiteral("stageOutcome"))
                                    || restored.value(QStringLiteral("writeOutcome"))
                                        != expected.value(QStringLiteral("writeOutcome"))
                                    || restored.value(QStringLiteral("checkpointOutcome"))
                                        != expected.value(QStringLiteral("checkpointOutcome")));
                          });
            }
        }
        for (const auto &boundary : durable) {
            suite.run("journal sync death during " + boundary.toStdString() + " keeps the completed record", [=] {
                QTemporaryDir directory;
                CHECK(directory.isValid());
                const auto child = runChild(boundary, directory.path(), QStringLiteral("journal"));
                requireChild(child, 42);
                bool incomplete = true;
                CHECK(replayed(directory.path(), &incomplete) == labeled(child.output, "EXPECTED"));
                CHECK(!incomplete);
            });
        }
        suite.run("payload sync death during edit keeps only the acknowledged draft", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto child = runChild(QStringLiteral("edit"), directory.path(), QStringLiteral("payload"));
            requireChild(child, 42);
            bool incomplete = true;
            const auto restored = replayed(directory.path(), &incomplete);
            CHECK(!incomplete);
            CHECK(restored == labeled(child.output, "BEFORE"));
            CHECK(restored.value(QStringLiteral("text"))
                  != labeled(child.output, "EXPECTED").value(QStringLiteral("text")));
        });
        suite.run("recovery-write retry keeps one edit and its decisions", [] {
            retryBoundary(
                openBase,
                [](test_support::TestSession &session) { return session.replaceDraft("file", 1, savedDraft()); },
                [](FileSession &file) { return file.replaceDraft(file.current().id, savedDraft()); });
        });
        suite.run("checkpoint sync failure retains the draft and retries one manual save", [] {
            retryBoundary(
                [](test_support::TestSession &session) {
                    openBase(session);
                    CHECK(session.replaceDraft("file", 1, savedDraft()) == Transition::Applied);
                },
                [](test_support::TestSession &session) {
                    return session.recordCheckpoint("file", session.file("file")->current().id, Outcome::Succeeded);
                },
                [](FileSession &file) { return file.recordCheckpoint(file.current().id, Outcome::Succeeded); });
        });
        suite.run("publication sync failure retains the reviewed draft and retries one write", [] {
            retryBoundary(
                reviewSaved,
                [](test_support::TestSession &session) {
                    return session.recordWrite("file", session.file("file")->current().id, Outcome::Succeeded);
                },
                [](FileSession &file) { return file.recordWrite(file.current().id, Outcome::Succeeded); });
        });
        // A recovery-journal fsync failure while recording a stage; the Git index is not involved.
        suite.run("stage outcome journal sync failure retains the written draft and retries one stage", [] {
            retryBoundary(
                writeSaved,
                [](test_support::TestSession &session) {
                    return session.recordStage("file", session.file("file")->current().id, Outcome::Succeeded,
                                               "verified-object", "100644");
                },
                [](FileSession &file) {
                    return file.recordStage(file.current().id, Outcome::Succeeded, "verified-object", "100644");
                });
        });
        suite.run("a corrupt record is not restored as success", [] {
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
        suite.run("an unsupported schema is not restored as success", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            requireChild(runChild(QStringLiteral("edit"), directory.path(), {}), 0);
            test_support::rewriteLastRecoveryState(
                directory.path(), [](QJsonObject &state) { state.insert(QStringLiteral("schema"), 99); });
            const auto replay = replayRecovery(storage::RecoveryStore(directory.path()));
            CHECK(!replay.succeeded && replay.files.empty() && replay.mutations.empty());
            CHECK(replay.error.contains(QStringLiteral("schema")));
        });
        suite.run("an unsupported journal header is not restored as success", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            requireChild(runChild(QStringLiteral("edit"), directory.path(), {}), 0);
            const auto journalPath = directory.path() + QStringLiteral("/journal");
            auto bytes = readFile(journalPath);
            CHECK(bytes.startsWith("Branch Truce recovery journal v2\n"));
            bytes.replace(0, sizeof("Branch Truce recovery journal v2\n") - 1, "Branch Truce recovery journal v9\n");
            writeFile(journalPath, bytes);
            const auto replay = replayRecovery(storage::RecoveryStore(directory.path()));
            CHECK(!replay.succeeded && replay.files.empty());
            CHECK(replay.newerFormat && replay.error.contains(QStringLiteral("newer Branch Truce")));
        });
        suite.run("a mismatched file identity is not restored as success", [] {
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
        suite.run("a stale destination rejects publication and keeps the acknowledged draft", [] {
            IndexConflict conflict;
            const QByteArray external = "external replacement\n";
            // Another program rewrites the file after the session captured it.
            writeFile(conflict.path(QStringLiteral("result.txt")), external);
            const auto conflicted = conflict.indexEntries();
            const auto revision = conflict.file().current().id;
            conflict.markResolved();
            const auto &file = conflict.file();
            CHECK(file.writeOutcome() && file.writeOutcome()->revision == revision
                  && file.writeOutcome()->outcome == Outcome::Failed);
            CHECK(QString::fromStdString(file.writeOutcome()->detail).contains(QLatin1String("content")));
            CHECK(!file.written() && !file.staged() && !file.facts().resolved);
            CHECK(file.current().draft.text == conflict.reviewed.toStdString());
            CHECK(readFile(conflict.path(QStringLiteral("result.txt"))) == external);
            CHECK(conflict.indexEntries() == conflicted);
            const auto replay = replayedJournal(conflict.controller->store()->recoveryDirectory());
            const auto *publication = journalMutation(replay, MutationKind::Publication);
            CHECK(publication && publication->observation == MutationObservation::NotAttempted
                  && !publication->completed);
            CHECK(observation(replay.files.front()) == observation(file));
        });
        // Git index faults run through the real publisher; the recovery journal syncs normally
        // and records what the product observed.
        suite.run("a replaced Git index entry stops publication and replay shows the recorded write failure", [] {
            IndexConflict conflict;
            const auto staleFile = conflict.home().path() + QStringLiteral("/stale-index-bytes");
            writeFile(staleFile, "stale index bytes\n");
            const auto stale = conflict
                                   .git({QStringLiteral("hash-object"), QStringLiteral("-w"),
                                         QStringLiteral("--no-filters"), staleFile})
                                   .trimmed();
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
            CHECK(QString::fromStdString(file.writeOutcome()->detail)
                      .contains(QLatin1String("index no longer matches this conflict")));
            CHECK(!file.stageOutcome() && !file.written() && !file.staged() && !file.facts().resolved);
            CHECK(file.facts().status == FileStatus::WriteFailed);
            CHECK(file.current().draft.text == conflict.reviewed.toStdString() && file.checkpoint());
            CHECK(readFile(conflict.path(QStringLiteral("result.txt"))) == conflict.manual);
            CHECK(conflict.indexEntries() == replacedIndex);
            CHECK(!conflict.controller->files()->hasPendingRecovery());
            const auto replay = replayedJournal(conflict.controller->store()->recoveryDirectory());
            CHECK(replay.mutations.empty());
            const auto &restored = replay.files.front();
            CHECK(observation(restored) == observation(file));
            CHECK(observation(restored).value(QStringLiteral("writeOutcome")).toString() == QLatin1String("failed"));
            CHECK(restored.writeOutcome() && restored.writeOutcome()->detail == file.writeOutcome()->detail);
        });
        suite.run("a locked Git index fails staging after the write and replay shows the recorded stage failure", [] {
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
            const auto replay = replayedJournal(conflict.controller->store()->recoveryDirectory());
            const auto *publication = journalMutation(replay, MutationKind::Publication);
            const auto *staging = journalMutation(replay, MutationKind::Staging);
            CHECK(publication && publication->observation == MutationObservation::ObservedChanged
                  && publication->completed);
            CHECK(staging && staging->observation == MutationObservation::ObservedUnchanged && !staging->completed);
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
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(application);
    const auto arguments = application.arguments();
    if (arguments.size() >= 5 && arguments.at(1) == QLatin1String("--boundary")) {
        QString boundary, directory, crash;
        for (int index = 1; index + 1 < arguments.size(); ++index) {
            if (arguments.at(index) == QLatin1String("--boundary"))
                boundary = arguments.at(++index);
            else if (arguments.at(index) == QLatin1String("--dir"))
                directory = arguments.at(++index);
            else if (arguments.at(index) == QLatin1String("--crash"))
                crash = arguments.at(++index);
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
