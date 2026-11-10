// The startup route suites' fixtures: conflicted repositories with an offline Git guard, the real
// binary's smoke-test launch, and the controller helpers every startup route file shares.
#pragma once

#include "TestHarness.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/EngineText.h"
#include "app/Translations.h"
#include "fixtures/AppSession.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/ConflictedRepository.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/ScopedEnvironment.h"
#include "fixtures/SyncFaults.h"
#include "mergetool/MergetoolProtocol.h"
#include "recovery/RecoveryRestore.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace startup_routes {
using namespace branch_truce;

// Fails the initial alignment of large.txt in the real load worker; other
// files align with the engine's algorithm.
class FailingLargeFile final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        if (result.starts_with("preserved large manual result\n")) {
            ++failures;
            throw std::bad_alloc();
        }
        return ExactLineCorrespondence{}.compare(source, result);
    }
    mutable std::atomic_int failures = 0;
};

// Records the thread and the result text of every alignment, so a case can tell on which thread
// each recovery store was replayed: replay aligns every captured file it restores.
class RecordingCorrespondence final : public CorrespondenceAlgorithm {
public:
    struct Call {
        std::string result;
        QThread *thread = nullptr;
    };
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        {
            const std::lock_guard lock(mutex_);
            if (recording_) calls_.push_back({std::string(result), QThread::currentThread()});
        }
        return ExactLineCorrespondence{}.compare(source, result);
    }
    void start() const
    {
        const std::lock_guard lock(mutex_);
        calls_.clear();
        recording_ = true;
    }
    std::vector<Call> stop() const
    {
        const std::lock_guard lock(mutex_);
        recording_ = false;
        return std::exchange(calls_, {});
    }

private:
    mutable std::mutex mutex_;
    mutable bool recording_ = false;
    mutable std::vector<Call> calls_;
};

using test_support::readFile;
using test_support::ScopedEnvironment;
using test_support::shellQuote;
using test_support::writeFile;

// tracked.txt and excluded.txt committed on base; conflict() stops an operation on them with a
// manual UTF-8 result in tracked.txt.
class Repository : public test_support::ConflictedRepository {
public:
    explicit Repository(bool excludedAttributes = false)
    {
        CHECK(executables.isValid());
        // The real binary runs offscreen.
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_QUICK_BACKEND", "software");
        QMap<QString, QByteArray> base{{"tracked.txt", "base\n"}, {"excluded.txt", "base\n"}};
        if (excludedAttributes) base.insert(".gitattributes", "excluded.txt -text eol=crlf\n");
        commit(base, "base");
    }
    // Stops `operation` ("merge", "rebase-merge", "rebase-apply" or "cherry-pick") on tracked.txt,
    // excluded.txt and the additional files, each given as its base, main and side text.
    void conflict(const QString &operation, const QMap<QString, std::array<QByteArray, 3>> &additional = {})
    {
        if (!additional.isEmpty()) {
            QMap<QString, QByteArray> base;
            for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry)
                base.insert(entry.key(), entry.value()[0]);
            commit(base, "additional base conflicts");
        }
        QMap<QString, QByteArray> mainFiles{{"tracked.txt", "main\n"}, {"excluded.txt", "main\n"}};
        QMap<QString, QByteArray> sideFiles{{"tracked.txt", "side\n"}, {"excluded.txt", "side\n"}};
        for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry) {
            mainFiles.insert(entry.key(), entry.value()[1]);
            sideFiles.insert(entry.key(), entry.value()[2]);
        }
        const auto stop = operation == "rebase-apply" ? Stop::RebaseApply
            : operation == "rebase-merge"             ? Stop::RebaseMerge
            : operation == "cherry-pick"              ? Stop::CherryPick
                                                      : Stop::Merge;
        std::tie(side, main) = ConflictedRepository::conflict(mainFiles, sideFiles, stop);
        writeFile(path("tracked.txt"), manual);
    }
    // Gives the repository an unreachable remote and puts a Git first on PATH that refuses every
    // network command and logs the rest.
    void offlineGuard()
    {
        git({"config", "remote.origin.url", "https://offline.invalid/never-contact"});
        git({"config", "branch.main.remote", "origin"});
        environment.insert("PATH", offline.directory() + ":/usr/bin:/bin");
    }
    QJsonObject launch(const QString &application, const QStringList &arguments, const QString &cwd = {}) const
    {
        const auto before = snapshot();
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(cwd.isEmpty() ? root : cwd);
        process.start(application, QStringList{"--smoke-test"} + arguments);
        CHECK(process.waitForStarted(5000));
        if (!process.waitForFinished(20000)) {
            process.kill();
            process.waitForFinished(5000);
            throw std::runtime_error("startup timed out: " + process.readAllStandardError().toStdString());
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            throw std::runtime_error("startup: " + process.readAllStandardError().toStdString());
        checkSocketDirectory();
        const auto output = process.readAllStandardOutput();
        QByteArray record;
        int records = 0;
        for (const auto &line : output.split('\n')) {
            if (line.startsWith("BRANCH_TRUCE_SESSION=")) {
                record = line.mid(21);
                ++records;
            }
        }
        CHECK(records == 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(record, &error);
        CHECK(error.error == QJsonParseError::NoError && document.isObject());
        CHECK(snapshot() == before);
        // A ready GUI listened, so it created the socket directory it actually used.
        if (document.object().value("state") == "ready") CHECK(QFileInfo(socketDirectory()).isDir());
        return document.object();
    }
    // The socket directory the GUI and the controller use for this fixture's environment.
    QString socketDirectory() const
    {
        return mergetoolSocketDirectory(environment.value(QStringLiteral("XDG_RUNTIME_DIR")).toUtf8());
    }
    void checkSocketDirectory() const
    {
        const auto socket = socketDirectory();
        if (socket != runtime.path() + QStringLiteral("/branch-truce"))
            throw std::runtime_error("socket directory outside the private runtime directory: " + socket.toStdString());
    }
    // Scratch space outside the worktree.
    QTemporaryDir executables;
    test_support::OfflineGit offline;
    QByteArray main;
    QByteArray side;
    const QByteArray manual = "manual UTF-8 \xc3\xa9 \xf0\x9f\x98\x80\n";
};

using test_support::waitOpened;

inline void openController(Application &controller, const Repository &repo, const QString &recoverySession = {})
{
    const auto before = repo.snapshot();
    repo.checkSocketDirectory();
    CHECK(mergetoolSocketDirectory(qgetenv("XDG_RUNTIME_DIR")) == repo.socketDirectory());
    test_support::openRepository(controller, repo.root, {}, recoverySession);
    if (controller.loader()->startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.loader()->detail().toStdString());
    CHECK(QFileInfo(repo.socketDirectory()).isDir());
    if (const auto changed = Repository::changed(before, repo.snapshot()); !changed.isEmpty())
        throw std::runtime_error("repository changed during startup: " + changed.join(", ").toStdString());
}

inline const FileSession &openedFile(Application &controller, const char *path = "tracked.txt")
{
    const auto *selected = controller.store()->file(QByteArray(path).toHex().toStdString());
    CHECK(selected != nullptr);
    return *selected;
}

inline QVariant fileRole(Application &controller, const FileSession &file, int role)
{
    auto *model = controller.files();
    for (int row = 0; row < model->totalFiles(); ++row)
        if (model->data(model->index(row), SessionModel::FileIdRole).toString().toStdString() == file.id())
            return model->data(model->index(row), role);
    throw std::runtime_error("fixture file has no model row");
}

// Files whose handoff entry names a blocker.
inline int blockedFiles(Application &controller)
{
    int count = 0;
    for (const auto &group : controller.completion()->handoff().groups)
        for (const auto &entry : group.entries)
            if (!entry.blockers.isEmpty()) ++count;
    return count;
}

inline QMap<QString, QByteArray> storeContents(const QString &root)
{
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        result.insert(QDir(root).relativeFilePath(path), readFile(path));
    }
    return result;
}

inline void checkMutationAdmission(Application &controller, const FileSession &failed, const FileSession &healthy)
{
    auto calls = std::make_shared<std::atomic_int>(0);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    std::vector<TaskCompletion> completions;
    auto *coordinator = controller.coordinator();
    QObject::connect(coordinator, &SessionCoordinator::taskFinished, &loop, [&](const TaskCompletion &completion) {
        completions.push_back(completion);
        if (completions.size() == 2) loop.quit();
    });
    const auto worker = [calls](const TaskInput &, const TaskControl &control) {
        ++*calls;
        if (!control.beginMutation())
            return TaskResult{TaskStatus::Cancelled, MutationObservation::NotAttempted, {}, {}, {}};
        return TaskResult{TaskStatus::Succeeded, MutationObservation::ObservedUnchanged, {}, {}, {}};
    };
    const auto inspect = [](const TaskInput &) { return MutationObservation::ObservedUnchanged; };
    CHECK(coordinator->submitMutation(failed.id(), failed.current().id, worker, inspect) != 0);
    CHECK(coordinator->submitMutation(healthy.id(), healthy.current().id, worker, inspect) != 0);
    timeout.start(10000);
    loop.exec();
    CHECK(completions.size() == 2 && calls->load() == 1 && coordinator->pendingTasks() == 0);
    for (const auto &completion : completions) {
        if (completion.stamp.fileId == failed.id()) {
            CHECK(completion.result.status == TaskStatus::Failed);
            CHECK(completion.result.mutation == MutationObservation::NotAttempted);
        } else {
            CHECK(completion.stamp.fileId == healthy.id() && completion.result.status == TaskStatus::Succeeded);
            CHECK(completion.result.mutation == MutationObservation::ObservedUnchanged);
        }
    }
}

inline SnapshotFileIdentity identity(const QString &path)
{
    struct stat observed{};
    CHECK(::lstat(QFile::encodeName(path).constData(), &observed) == 0);
    return {static_cast<std::uint64_t>(observed.st_dev),
            static_cast<std::uint64_t>(observed.st_ino),
            static_cast<std::uint32_t>(observed.st_mode),
            static_cast<std::int64_t>(observed.st_size),
            observed.st_mtim.tv_sec,
            observed.st_mtim.tv_nsec,
            observed.st_ctim.tv_sec,
            observed.st_ctim.tv_nsec};
}

inline QJsonObject file(const QJsonObject &state, const QString &name)
{
    const auto files = state.value("files").toArray();
    for (const auto &value : files)
        if (value.toObject().value("path").toString() == name) return value.toObject();
    throw std::runtime_error("missing startup file: " + name.toStdString());
}

inline void ready(const Repository &repo, const QJsonObject &state, const QString &operation)
{
    CHECK(state.value("state") == "ready" && state.value("operation") == operation);
    CHECK(state.value("repository").toString() == repo.root && state.value("files").toArray().size() == 2);
    CHECK(state.value("resolved").toInt() == 0 && !state.value("workflow").toString().isEmpty());
    CHECK(file(state, "tracked.txt").value("support") == "supported");
    CHECK(file(state, "tracked.txt").value("text").toString().toUtf8() == repo.manual);
    CHECK(file(state, "tracked.txt").value("detail").toString().isEmpty());
    CHECK(state.value("stage2CommitId").toString() == QString::fromLatin1(repo.main));
    CHECK(state.value("stage3CommitId").toString() == QString::fromLatin1(repo.side));
}

// Child mode of this executable: opens the repository at root, edits tracked.txt and kills itself
// while it holds the store's lease. Prints ACK after the edit is acknowledged.
int openEditAndCrash(const QString &root);

// Each file registers its cases; application is the real branch-truce binary.
void addPublicationCases(test_support::Suite &suite, const QString &application);
void addRecoveryCases(test_support::Suite &suite, const QString &application);
void addCaptureCases(test_support::Suite &suite, const QString &application);
void addLaunchCases(test_support::Suite &suite, const QString &application);
} // namespace startup_routes
