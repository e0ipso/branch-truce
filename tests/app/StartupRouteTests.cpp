#include "app/ApplicationController.h"
#include "app/RecoveryRestore.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/RecoveryJournal.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <array>
#include <atomic>
#include <sys/stat.h>

using namespace branch_truce;

namespace correspondence_faults {
inline std::atomic_bool failLarge = false;
inline std::atomic_int failures = 0;
struct Failure {
    Failure() { failures = 0; failLarge = true; }
    ~Failure() { failLarge = false; }
};
}

// Itanium constructor ABI used by the project's Linux GCC/Clang toolchains.
#define MAP_CONSTRUCTOR "_ZN12branch_truce17CorrespondenceMapC1EmRKNS_13FileSnapshotsENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt10shared_ptrIKNS_23CorrespondenceAlgorithmEE"
extern "C" void realMap(CorrespondenceMap *, RevisionId, const FileSnapshots &, std::string,
    std::shared_ptr<const CorrespondenceAlgorithm>) asm("__real_" MAP_CONSTRUCTOR);
extern "C" void wrapMap(CorrespondenceMap *, RevisionId, const FileSnapshots &, std::string,
    std::shared_ptr<const CorrespondenceAlgorithm>) asm("__wrap_" MAP_CONSTRUCTOR);
extern "C" void wrapMap(CorrespondenceMap *self, RevisionId revision, const FileSnapshots &sources,
    std::string result, std::shared_ptr<const CorrespondenceAlgorithm> algorithm)
{
    if (correspondence_faults::failLarge && sources.path == "large.txt") {
        ++correspondence_faults::failures;
        throw std::runtime_error("injected large-file correspondence resource failure");
    }
    realMap(self, revision, sources, std::move(result), std::move(algorithm));
}
#undef MAP_CONSTRUCTOR

namespace {
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    CHECK(file.error() == QFileDevice::NoError);
    return bytes;
}

QByteArray quoteShell(QByteArray value)
{
    value.replace("'", "'\\''");
    return '\'' + value + '\'';
}

class Repository {
public:
    explicit Repository(bool excludedAttributes = false)
    {
        CHECK(directory.isValid() && executables.isValid());
        root = QFileInfo(directory.path()).canonicalFilePath();
        environment.insert("PATH", "/usr/bin:/bin");
        // App caches and user configuration must stay outside the launched worktree.
        environment.insert("HOME", executables.path());
        environment.insert("XDG_CONFIG_HOME", executables.path() + "/config");
        environment.insert("XDG_CACHE_HOME", executables.path() + "/cache");
        environment.insert("XDG_DATA_HOME", executables.path() + "/data");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_QUICK_BACKEND", "software");
        git({"init", "--quiet", "--initial-branch=main"});
        git({"config", "user.name", "Branch Truce Tests"});
        git({"config", "user.email", "tests@example.invalid"});
        writeFile(path("tracked.txt"), "base\n");
        writeFile(path("excluded.txt"), "base\n");
        if (excludedAttributes) {
            writeFile(path(".gitattributes"), "excluded.txt -text eol=crlf\n");
            git({"add", "--", ".gitattributes"});
        }
        git({"add", "--", "tracked.txt", "excluded.txt"});
        git({"commit", "--quiet", "-m", "base"});
    }
    QString path(const QString &relative) const { return root + '/' + relative; }
    QByteArray git(const QStringList &arguments, int expected = 0) const
    {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start("/usr/bin/git", QStringList{"--no-pager", "--literal-pathspecs"} + arguments);
        CHECK(process.waitForStarted(5000) && process.waitForFinished(5000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    void conflict(const QString &operation, const QMap<QString, std::array<QByteArray, 3>> &additional = {})
    {
        if (!additional.isEmpty()) {
            for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry)
                writeFile(path(entry.key()), entry.value()[0]);
            git(QStringList{"add", "--"} + additional.keys());
            git({"commit", "--quiet", "-m", "additional base conflicts"});
        }
        git({"checkout", "--quiet", "-b", "side"});
        for (const auto name : {"tracked.txt", "excluded.txt"}) writeFile(path(name), "side\n");
        for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry)
            writeFile(path(entry.key()), entry.value()[2]);
        git(QStringList{"add", "--", "tracked.txt", "excluded.txt"} + additional.keys());
        git({"commit", "--quiet", "-m", "side"});
        side = git({"rev-parse", "HEAD"}).trimmed();
        git({"checkout", "--quiet", "main"});
        for (const auto name : {"tracked.txt", "excluded.txt"}) writeFile(path(name), "main\n");
        for (auto entry = additional.cbegin(); entry != additional.cend(); ++entry)
            writeFile(path(entry.key()), entry.value()[1]);
        git(QStringList{"add", "--", "tracked.txt", "excluded.txt"} + additional.keys());
        git({"commit", "--quiet", "-m", "main"});
        main = git({"rev-parse", "HEAD"}).trimmed();
        if (operation.startsWith("rebase")) {
            git({"checkout", "--quiet", "side"});
            git({"rebase", operation == "rebase-apply" ? "--apply" : "--merge", "main"}, 1);
        } else if (operation == "cherry-pick") {
            git({"cherry-pick", "side"}, 1);
        } else {
            git({"merge", "--no-edit", "side"}, 1);
        }
        writeFile(path("tracked.txt"), manual);
    }
    QMap<QString, QByteArray> snapshot() const
    {
        QMap<QString, QByteArray> result;
        // Includes the index, operation metadata, refs, objects, and untracked files.
        QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto selected = files.next();
            result.insert(QDir(root).relativeFilePath(selected), readFile(selected));
        }
        return result;
    }
    void offlineGuard()
    {
        git({"config", "remote.origin.url", "https://offline.invalid/never-contact"});
        git({"config", "branch.main.remote", "origin"});
        const auto log = quoteShell(QFile::encodeName(executables.path() + "/git-argv"));
        const auto marker = quoteShell(QFile::encodeName(executables.path() + "/network-attempt"));
        const auto script = executables.path() + "/git";
        writeFile(script, "#!/bin/sh\nfor argument in \"$@\"; do\n"
            "  case \"$argument\" in fetch|pull|push|clone|ls-remote|remote|submodule) : > " + marker + "; exit 97 ;; esac\n"
            "done\nprintf '%s\\n' \"$@\" >> " + log + "\nexec /usr/bin/git \"$@\"\n");
        CHECK(QFile::setPermissions(script, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        environment.insert("PATH", executables.path() + ":/usr/bin:/bin");
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
        const auto output = process.readAllStandardOutput();
        QByteArray record;
        int records = 0;
        for (const auto &line : output.split('\n')) {
            if (line.startsWith("BRANCH_TRUCE_SESSION=")) { record = line.mid(21); ++records; }
        }
        CHECK(records == 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(record, &error);
        CHECK(error.error == QJsonParseError::NoError && document.isObject());
        CHECK(snapshot() == before);
        return document.object();
    }
    QTemporaryDir directory;
    QTemporaryDir executables;
    QString root;
    QProcessEnvironment environment;
    QByteArray main;
    QByteArray side;
    const QByteArray manual = "manual UTF-8 \xc3\xa9 \xf0\x9f\x98\x80\n";
};

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const QProcessEnvironment &environment)
    {
        for (const auto name : {"HOME", "PATH", "LC_ALL", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                "XDG_DATA_HOME", "GIT_CONFIG_NOSYSTEM", "GIT_TERMINAL_PROMPT"}) {
            previous.insert(name, qEnvironmentVariableIsSet(name)
                ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt);
            qputenv(name, environment.value(QString::fromLatin1(name)).toUtf8());
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

void openController(ApplicationController &controller, const Repository &repo)
{
    const auto before = repo.snapshot();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &ApplicationController::launchChanged, &loop, [&] {
        if (!controller.opening()) loop.quit();
    });
    CHECK(controller.openRepository(repo.root));
    timeout.start(10000);
    if (controller.opening()) loop.exec();
    CHECK(!controller.opening());
    if (controller.startupState() != "ready")
        throw std::runtime_error("controller startup: " + controller.detail().toStdString());
    CHECK(repo.snapshot() == before);
}

const FileSession &openedFile(ApplicationController &controller, const char *path = "tracked.txt")
{
    const auto *selected = controller.files()->file(QByteArray(path).toHex().toStdString());
    CHECK(selected != nullptr);
    return *selected;
}

QVariant fileRole(ApplicationController &controller, const FileSession &file, int role)
{
    auto *model = controller.files();
    for (int row = 0; row < model->totalFiles(); ++row)
        if (model->data(model->index(row), SessionModel::FileIdRole).toString().toStdString() == file.id())
            return model->data(model->index(row), role);
    throw std::runtime_error("fixture file has no model row");
}

QMap<QString, QByteArray> storeContents(const QString &root)
{
    QMap<QString, QByteArray> result;
    QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        result.insert(QDir(root).relativeFilePath(path), readFile(path));
    }
    return result;
}

void checkMutationAdmission(ApplicationController &controller, const FileSession &failed, const FileSession &healthy)
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

SnapshotFileIdentity identity(const QString &path)
{
    struct stat observed {};
    CHECK(::lstat(QFile::encodeName(path).constData(), &observed) == 0);
    return {static_cast<std::uint64_t>(observed.st_dev), static_cast<std::uint64_t>(observed.st_ino),
        static_cast<std::uint32_t>(observed.st_mode), static_cast<std::int64_t>(observed.st_size),
        observed.st_mtim.tv_sec, observed.st_mtim.tv_nsec, observed.st_ctim.tv_sec, observed.st_ctim.tv_nsec};
}

QJsonObject file(const QJsonObject &state, const QString &name)
{
    const auto files = state.value("files").toArray();
    for (const auto &value : files) if (value.toObject().value("path").toString() == name) return value.toObject();
    throw std::runtime_error("missing startup file: " + name.toStdString());
}

void ready(const Repository &repo, const QJsonObject &state, const QString &operation)
{
    CHECK(state.value("state") == "ready" && state.value("operation") == operation);
    CHECK(state.value("repository").toString() == repo.root && state.value("files").toArray().size() == 2);
    CHECK(state.value("resolved").toInt() == 0 && !state.value("workflow").toString().isEmpty());
    CHECK(file(state, "tracked.txt").value("support") == "supported");
    CHECK(file(state, "tracked.txt").value("text").toString().toUtf8() == repo.manual);
    CHECK(file(state, "tracked.txt").value("detail").toString().isEmpty());
    CHECK(state.value("stage2").toString().contains(QString::fromLatin1(repo.main)));
    CHECK(state.value("stage3").toString().contains(QString::fromLatin1(repo.side)));
}
} // namespace

class StartupRouteTests {
public:
    int run(const QString &application)
    {
        test_support::Suite suite("StartupRouteTests");
        suite.run("TRUCE-0014 restart restores acknowledged unsaved work independently of Save review and resolution", [] {
            Repository repo;
            repo.conflict("merge");
            ScopedEnvironment environment(repo.environment);
            QString location;
            Draft saved, unsaved;
            std::optional<FileSnapshots> captured;
            {
                ApplicationController controller;
                openController(controller, repo);
                auto *model = controller.files();
                const auto &selected = openedFile(controller);
                captured = *selected.snapshots();
                const auto end = selected.current().draft.text.size();
                CHECK(model->editResult(selected.id(), selected.current().id, {{end, end}, "saved app draft\n"}) == Transition::Applied);
                saved = selected.current().draft;
                CHECK(model->recordCheckpoint(selected.id(), selected.current().id, Outcome::Succeeded) == Transition::Applied);
                const auto savedEnd = saved.text.size();
                CHECK(model->editResult(selected.id(), selected.current().id, {{savedEnd, savedEnd}, "acknowledged unsaved work\n"}) == Transition::Applied);
                unsaved = selected.current().draft;
                location = model->recoveryDirectory();
                CHECK(!model->hasPendingRecovery());
            }
            for (int restart = 0; restart < 2; ++restart) {
                ApplicationController controller;
                openController(controller, repo);
                const auto &restored = openedFile(controller);
                CHECK(controller.files()->recoveryDirectory() == location);
                CHECK(restored.current().draft == unsaved && *restored.snapshots() == *captured);
                CHECK(restored.checkpoint() && restored.checkpoint()->draft == saved);
                CHECK(restored.facts().unsaved && !restored.facts().resolved && restored.facts().needsReview);
                CHECK(!restored.reviewed() && !restored.written() && !restored.staged());
                CHECK(fileRole(controller, restored, SessionModel::RecoveryStateRole).toString() == "restored");
                CHECK(!fileRole(controller, restored, SessionModel::PendingInputRole).toBool());
                CHECK(!controller.files()->hasRecoveryFailures() && !controller.files()->canFinish());
            }
        });
        suite.run("TRUCE-0014 an incomplete final record restores only acknowledged work with a pending-input diagnostic", [] {
            Repository repo;
            repo.conflict("merge");
            ScopedEnvironment environment(repo.environment);
            QString location;
            Draft acknowledged;
            {
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(controller.files()->editResult(selected.id(), 1, {{0, 0}, "acknowledged\n"}) == Transition::Applied);
                acknowledged = selected.current().draft;
                location = controller.files()->recoveryDirectory();
            }
            QFile journal(location + "/journal");
            CHECK(journal.open(QIODevice::Append));
            CHECK(journal.write("{\"interrupted pending input\":", 29) == 29);
            journal.close();
            ApplicationController controller;
            openController(controller, repo);
            const auto &restored = openedFile(controller);
            CHECK(restored.current().draft == acknowledged && restored.facts().unsaved);
            CHECK(controller.detail().contains("Pending input may need repeating"));
            CHECK(!controller.files()->hasPendingRecovery() && !controller.files()->hasRecoveryFailures());
            CHECK(fileRole(controller, restored, SessionModel::RecoveryStateRole).toString() == "restored");
        });
        for (const auto &condition : {QString("stale"), QString("corrupt"), QString("missing-payload"),
                                      QString("corrupt-payload"), QString("schema"), QString("locked")}) {
            suite.run("TRUCE-0054 unadopted " + condition.toStdString() + " store keeps diagnostics and every live file editable", [=] {
                Repository repo;
                repo.conflict("merge");
                ScopedEnvironment environment(repo.environment);
                QString abandoned;
                const QByteArray oldDraft = "old acknowledged unsaved work\n";
                {
                    ApplicationController controller;
                    openController(controller, repo);
                    const auto &selected = openedFile(controller);
                    auto edited = selected.current().draft;
                    edited.text = oldDraft.toStdString();
                    CHECK(controller.files()->replaceDraft(selected.id(), 1, edited) == Transition::Applied);
                    abandoned = controller.files()->recoveryDirectory();
                }
                auto lock = std::make_unique<QLockFile>(abandoned + "/session.lock");
                QString diagnostic;
                if (condition == "stale") {
                    repo.git({"merge", "--abort"});
                    writeFile(repo.path("tracked.txt"), "second main\n");
                    repo.git({"add", "--", "tracked.txt"});
                    repo.git({"commit", "--quiet", "-m", "second operation"});
                    repo.git({"merge", "--no-edit", "side"}, 1);
                    writeFile(repo.path("tracked.txt"), "second manual result\n");
                    diagnostic = "session identity does not match";
                } else if (condition == "locked") {
                    CHECK(lock->tryLock(0));
                    diagnostic = "active or unavailable";
                } else if (condition == "schema") {
                    test_support::rewriteLastRecoveryState(abandoned, [](QJsonObject &state) { state.insert("schema", 99); });
                    diagnostic = "schema";
                } else if (condition == "corrupt") {
                    QFile journal(abandoned + "/journal");
                    CHECK(journal.open(QIODevice::Append) && journal.write("corrupt complete frame\n") > 0);
                    diagnostic = "integrity";
                } else {
                    const auto path = abandoned + "/payloads/" + QString::fromLatin1(storage::RecoveryStore::digest(oldDraft));
                    if (condition == "missing-payload") CHECK(QFile::remove(path));
                    else writeFile(path, "different payload bytes");
                    diagnostic = "payload";
                }
                ApplicationController controller;
                openController(controller, repo);
                const auto detail = controller.detail();
                CHECK(detail.contains(diagnostic) && controller.files()->recoveryDirectory() != abandoned);
                CHECK(!controller.files()->hasRecoveryFailures() && !controller.files()->hasPendingRecovery());
                CHECK(openedFile(controller).current().draft.text != oldDraft.toStdString());
                for (const auto path : {"tracked.txt", "excluded.txt"}) {
                    const auto &selected = openedFile(controller, path);
                    CHECK(!controller.files()->hasRecoveryFailure(selected.id()));
                    const auto id = QString::fromStdString(selected.id());
                    CHECK(controller.selectFile(id) && controller.selectedFile().value("canEdit").toBool());
                    CHECK(controller.editResult(id, QString::number(selected.current().id), 0, 0, "live manual edit\n"));
                    CHECK(controller.undo(id, QString::number(selected.current().id)));
                    CHECK(controller.redo(id, QString::number(selected.current().id)));
                    CHECK(controller.detail() == detail && !controller.files()->hasPendingRecovery());
                }
            });
        }
        suite.run("TRUCE-0054 manual retirement outside v1 preserves the complete store and is reversible", [] {
            Repository repo;
            repo.conflict("merge");
            ScopedEnvironment environment(repo.environment);
            QString abandoned;
            {
                ApplicationController controller;
                openController(controller, repo);
                abandoned = controller.files()->recoveryDirectory();
            }
            QFile journal(abandoned + "/journal");
            CHECK(journal.open(QIODevice::Append) && journal.write("corrupt complete frame\n") > 0);
            journal.close();
            const auto retained = storeContents(abandoned);
            CHECK(retained.contains("journal") && retained.size() > 1);
            const auto retired = repo.executables.path() + "/retired-session";
            CHECK(!retired.startsWith(QFileInfo(abandoned).absolutePath() + '/'));
            CHECK(QDir().rename(abandoned, retired) && !QFileInfo::exists(abandoned));
            CHECK(storeContents(retired) == retained);
            {
                ApplicationController controller;
                openController(controller, repo);
                CHECK(!controller.detail().contains("rejected") && !controller.files()->hasRecoveryFailures());
                const auto &selected = openedFile(controller);
                CHECK(controller.files()->editResult(selected.id(), selected.current().id, {{0, 0}, "live edit\n"}) == Transition::Applied);
            }
            CHECK(QDir().rename(retired, abandoned) && storeContents(abandoned) == retained);
            ApplicationController controller;
            openController(controller, repo);
            CHECK(controller.detail().contains("integrity") && !controller.files()->hasRecoveryFailures());
            CHECK(openedFile(controller).current().draft.text.starts_with("live edit\n"));
        });
        suite.run("TRUCE-0055 external resolution blocks only its file across reopenings choices history and mutation admission", [] {
            Repository repo;
            repo.conflict("merge");
            writeFile(repo.path("excluded.txt"), "main\n");
            ScopedEnvironment environment(repo.environment);
            Draft recovered;
            QString location;
            {
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(controller.files()->editResult(selected.id(), 1, {{0, 0}, "acknowledged draft\n"}) == Transition::Applied);
                recovered = selected.current().draft;
                location = controller.files()->recoveryDirectory();
            }
            writeFile(repo.path("tracked.txt"), "resolved outside the app\n");
            repo.git({"add", "--", "tracked.txt"});
            for (int restart = 0; restart < 2; ++restart) {
                ApplicationController controller;
                openController(controller, repo);
                auto *model = controller.files();
                const auto &failed = openedFile(controller);
                const auto &healthy = openedFile(controller, "excluded.txt");
                CHECK(model->recoveryDirectory() == location && model->totalFiles() == 2);
                CHECK(model->hasRecoveryFailure(failed.id()) && !model->hasRecoveryFailure(healthy.id()));
                CHECK(!model->hasPendingRecovery() && !model->canFinish());
                CHECK(failed.current().draft == recovered && failed.canUndo());
                CHECK(fileRole(controller, failed, SessionModel::RecoveryStateRole).toString() == "blocked");
                CHECK(!fileRole(controller, failed, SessionModel::RecoveryErrorRole).toString().isEmpty());
                CHECK(!fileRole(controller, failed, SessionModel::PendingInputRole).toBool());
                CHECK(!fileRole(controller, failed, SessionModel::CanUndoRole).toBool());
                const auto id = QString::fromStdString(failed.id());
                CHECK(controller.selectFile(id) && !controller.selectedFile().value("canEdit").toBool());
                CHECK(!controller.editResult(id, QString::number(failed.current().id), 0, 0, "forbidden"));
                CHECK(!controller.undo(id, QString::number(failed.current().id)));
                CHECK(!controller.redo(id, QString::number(failed.current().id)));
                const auto target = test_support::choiceTarget(failed, SourceSide::Stage2, ChoiceScope::Section);
                CHECK(!controller.previewChoice(id, QString::number(failed.current().id),
                    QString::fromStdString(target.source.contributionId), "accept"));
                const auto preview = previewChoice(failed, failed.current().id, target.source.contributionId, ChoiceAction::Accept);
                CHECK(model->applyChoice(failed.id(), preview, true) == Transition::Blocked);
                CHECK(model->replaceDraft(failed.id(), failed.current().id, {"forbidden", {}, 0}) == Transition::Blocked);
                CHECK(model->editResult(failed.id(), failed.current().id, {{0, 0}, "forbidden"}) == Transition::Blocked);
                CHECK(model->undo(failed.id(), failed.current().id) == Transition::Blocked);
                CHECK(model->redo(failed.id(), failed.current().id) == Transition::Blocked);
                const auto healthyId = QString::fromStdString(healthy.id());
                CHECK(controller.selectFile(healthyId) && controller.selectedFile().value("canEdit").toBool());
                CHECK(controller.editResult(healthyId, QString::number(healthy.current().id), 0, 0, "healthy manual edit\n"));
                const auto before = healthy.current().draft;
                const auto healthyTarget = test_support::choiceTarget(healthy, SourceSide::Stage2, ChoiceScope::Line);
                CHECK(controller.previewChoice(healthyId, QString::number(healthy.current().id),
                    QString::fromStdString(healthyTarget.source.contributionId), "accept"));
                CHECK(controller.applyChoice(true));
                const auto chosen = healthy.current().draft;
                CHECK(chosen != before && chosen.text == before.text);
                CHECK(controller.undo(healthyId, QString::number(healthy.current().id)) && healthy.current().draft == before);
                CHECK(controller.redo(healthyId, QString::number(healthy.current().id)) && healthy.current().draft == chosen);
                CHECK(controller.undo(healthyId, QString::number(healthy.current().id)));
                CHECK(fileRole(controller, healthy, SessionModel::CanUndoRole).toBool());
                const auto repositoryBefore = repo.snapshot();
                checkMutationAdmission(controller, failed, healthy);
                CHECK(repo.snapshot() == repositoryBefore);
                CHECK(model->hasRecoveryFailure(failed.id()) && !model->hasRecoveryFailure(healthy.id()));
                CHECK(!fileRole(controller, failed, SessionModel::RecoveryErrorRole).toString().isEmpty());
            }
        });
        suite.run("TRUCE-0016 earlier recovered drafts retain their decision vocabulary and direct editing", [] {
            Repository repo;
            repo.conflict("merge");
            ScopedEnvironment environment(repo.environment);
            QString location;
            std::optional<FileSession> captured;
            {
                ApplicationController controller;
                openController(controller, repo);
                captured = openedFile(controller);
                location = controller.files()->recoveryDirectory();
            }
            CHECK(QDir(location).removeRecursively());
            const std::map<std::string, Decision> vocabulary{{"left", {DecisionValue::Accepted, 1}}, {"right", {}}};
            const FileSession legacy(captured->id(), captured->path(), {captured->current().draft.text, vocabulary, 0},
                Support::Supported, {}, *captured->snapshots());
            CHECK(storage::RecoveryStore(location).commit(recoveryTransaction(legacy, "open", QUuid::createUuid().toString())).succeeded);
            for (int restart = 0; restart < 2; ++restart) {
                ApplicationController controller;
                openController(controller, repo);
                const auto &restored = openedFile(controller);
                CHECK(controller.files()->recoveryDirectory() == location);
                CHECK(restored.current().draft.decisions == vocabulary && restored.facts().undecided == 1);
                CHECK(!restored.choices().empty());
                const auto target = test_support::choiceTarget(restored, SourceSide::Stage2, ChoiceScope::Section);
                CHECK(previewChoice(restored, restored.current().id, target.source.contributionId, ChoiceAction::Accept).state == Transition::Blocked);
                const auto id = QString::fromStdString(restored.id());
                CHECK(controller.selectFile(id) && controller.selectedFile().value("canEdit").toBool());
                CHECK(controller.selectedFile().value("choices").toList().isEmpty());
                CHECK(!controller.selectedFile().value("choiceNotice").toString().isEmpty());
                CHECK(!controller.previewChoice(id, QString::number(restored.current().id),
                    QString::fromStdString(target.source.contributionId), "accept"));
                CHECK(controller.editResult(id, QString::number(restored.current().id), 0, 0, "legacy manual edit\n"));
                CHECK(restored.current().draft.decisions == vocabulary);
                CHECK(controller.undo(id, QString::number(restored.current().id)));
                CHECK(restored.current().draft.decisions == vocabulary);
            }
        });
        for (const auto &kind : {QString("publication"), QString("staging")}) {
            for (const bool resultPresent : {false, true}) {
                suite.run("TRUCE-0014 interrupted " + kind.toStdString() + " remains uncertain against current disk and index "
                    + std::to_string(resultPresent), [=] {
                    Repository repo;
                    repo.conflict("merge");
                    ScopedEnvironment environment(repo.environment);
                    const QByteArray candidate = "reviewed result awaiting completion\n";
                    QString location;
                    {
                        ApplicationController controller;
                        openController(controller, repo);
                        auto *model = controller.files();
                        const auto &selected = openedFile(controller);
                        auto edited = selected.current().draft;
                        edited.text = candidate.toStdString();
                        CHECK(model->replaceDraft(selected.id(), 1, edited) == Transition::Applied);
                        const auto revision = selected.current().id;
                        CHECK(model->beginMarkResolved(selected.id(), revision, true) == Transition::Applied);
                        CHECK(model->recordCheckpoint(selected.id(), revision, Outcome::Succeeded) == Transition::Applied);
                        if (kind == "staging") {
                            writeFile(repo.path("tracked.txt"), candidate);
                            CHECK(model->recordWrite(selected.id(), revision, Outcome::Succeeded) == Transition::Applied);
                        }
                        location = model->recoveryDirectory();
                        CHECK(storage::RecoveryStore(location).commit(mutationRecoveryTransaction(selected, kind,
                            QUuid::createUuid().toString())).succeeded);
                    }
                    if (resultPresent) writeFile(repo.path("tracked.txt"), candidate);
                    else if (kind == "staging") {
                        writeFile(repo.path("tracked.txt"), "different externally staged result\n");
                        repo.git({"add", "--", "tracked.txt"});
                    }
                    const auto repositoryBefore = repo.snapshot();
                    ApplicationController controller;
                    openController(controller, repo);
                    const auto &restored = openedFile(controller);
                    const auto diagnostic = fileRole(controller, restored, SessionModel::RecoveryErrorRole).toString();
                    CHECK(diagnostic.contains("Interrupted " + kind) && diagnostic.contains("uncertain", Qt::CaseInsensitive));
                    CHECK(diagnostic.contains(resultPresent ? "result bytes are present" : "cannot establish completion"));
                    CHECK(restored.current().draft.text == candidate.toStdString());
                    CHECK(!restored.facts().resolved && !restored.staged());
                    CHECK(controller.files()->hasRecoveryFailure(restored.id()) && !controller.files()->hasPendingRecovery());
                    CHECK(fileRole(controller, restored, SessionModel::RecoveryStateRole).toString() == "blocked");
                    CHECK(controller.files()->editResult(restored.id(), restored.current().id, {{0, 0}, "must inspect"}) == Transition::Blocked);
                    CHECK(repo.snapshot() == repositoryBefore);
                    const auto replay = replayRecovery(storage::RecoveryStore(location));
                    CHECK(replay.succeeded && replay.mutations.size() == 1 && replay.mutations.front().observation.isEmpty());
                });
            }
        }
        suite.run("TRUCE-0023 mixed inventory retains every exact path sources initial results and persistent outstanding reasons", [] {
            Repository repo;
            const QMap<QString, std::array<QByteArray, 3>> additional{
                {":literal\tfile.txt", {"tab base\n", "tab main\n", "tab side\n"}},
                {":literal\\tfile.txt", {"slash base\n", "slash main\n", "slash side\n"}},
                {"binary.dat", {QByteArray("base\0binary", 11), QByteArray("main\0binary", 11), QByteArray("side\0binary", 11)}},
                {"missing.txt", {"missing base\n", "missing main\n", "missing side\n"}}
            };
            repo.conflict("merge", additional);
            for (const auto &path : {QString(":literal\tfile.txt"), QString(":literal\\tfile.txt")})
                writeFile(repo.path(path), "preserved manual result for " + path.toUtf8() + '\n');
            CHECK(QFile::remove(repo.path("missing.txt")));
            const auto unmerged = repo.git({"diff", "--name-only", "--diff-filter=U", "-z"}).split('\0');
            QSet<QByteArray> expectedPaths;
            for (const auto &path : unmerged) if (!path.isEmpty()) expectedPaths.insert(path);
            CHECK(expectedPaths.size() == 6);
            const auto before = repo.snapshot();
            ScopedEnvironment environment(repo.environment);
            ApplicationController controller;
            openController(controller, repo);
            auto *model = controller.files();
            CHECK(model->totalFiles() == 6 && controller.outsideAppFiles() == 1 && controller.loadErrorFiles() == 1);
            QSet<QByteArray> actualPaths;
            for (int row = 0; row < model->totalFiles(); ++row) {
                const auto index = model->index(row);
                const auto path = model->data(index, SessionModel::PathBytesRole).toByteArray();
                const auto id = model->data(index, SessionModel::FileIdRole).toString();
                actualPaths.insert(path);
                CHECK(id.toLatin1() == path.toHex() && controller.selectFile(id));
                const auto selected = controller.selectedFile();
                CHECK(selected.value("pathBytes").toByteArray() == path && selected.value("fileId").toString() == id);
                if (path == "binary.dat" || path == "missing.txt") {
                    const auto expected = path == "binary.dat" ? "outside-app" : "unreadable";
                    CHECK(selected.value("support").toString() == QLatin1String(expected));
                    CHECK(!selected.value("supportDetail").toString().isEmpty());
                    CHECK(!selected.contains("stage2") && !selected.contains("initialResult"));
                    CHECK(model->replaceDraft(id.toStdString(), 1, {"unsafe edit", {}, 0}) == Transition::Blocked);
                    continue;
                }
                CHECK(selected.value("support").toString() == "supported");
                const auto versions = additional.value(QString::fromUtf8(path), {"base\n", "main\n", "side\n"});
                CHECK(selected.value("earlier").toMap().value("text").toString().toUtf8() == versions[0]);
                CHECK(selected.value("stage2").toMap().value("text").toString().toUtf8() == versions[1]);
                CHECK(selected.value("stage3").toMap().value("text").toString().toUtf8() == versions[2]);
                const auto initial = readFile(repo.path(QString::fromUtf8(path)));
                CHECK(selected.value("initialResult").toMap().value("text").toString().toUtf8() == initial);
                CHECK(selected.value("resultText").toString().toUtf8() == initial);
                CHECK(!controller.selectFile(QString::fromUtf8(path)) && controller.selectedFileId() == id);
            }
            CHECK(actualPaths == expectedPaths);
            const auto selectedId = controller.selectedFileId();
            CHECK(!controller.selectFile("not-an-exact-file-id") && controller.selectedFileId() == selectedId);
            // Adapter success callbacks complete supported rows without touching Git in this fixture.
            for (int row = 0; row < model->totalFiles(); ++row) {
                const auto id = model->data(model->index(row), SessionModel::FileIdRole).toString().toStdString();
                if (model->file(id)->support() != Support::Supported) continue;
                CHECK(model->replaceDraft(id, 1, {"reviewed supported result\n", {}, 0}) == Transition::Applied);
                const auto revision = model->file(id)->current().id;
                CHECK(model->beginMarkResolved(id, revision) == Transition::Applied);
                CHECK(model->recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
                CHECK(model->recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
                CHECK(model->recordStage(id, revision, Outcome::Succeeded, "fixture-verified-object", "100644") == Transition::Applied);
            }
            CHECK(model->resolvedFiles() == 4 && !model->canFinish() && model->finishBlockers().size() == 2);
            CHECK(controller.outsideAppFiles() == 1 && controller.loadErrorFiles() == 1);
            CHECK(controller.detail().contains("outside app remain outstanding"));
            for (const auto path : {"binary.dat", "missing.txt"}) {
                const auto &outstanding = openedFile(controller, path);
                CHECK(!outstanding.supportDetail().empty() && !outstanding.facts().resolved);
                CHECK(controller.selectFile(QString::fromStdString(outstanding.id())));
                CHECK(!controller.selectedFile().value("supportDetail").toString().isEmpty());
            }
            CHECK(repo.snapshot() == before);
        });
        suite.run("TRUCE-0052 a large admitted file correspondence failure does not abort loading the small files", [] {
            Repository repo;
            QByteArray large;
            for (int line = 0; line < 5000; ++line) large += "large line " + QByteArray::number(line) + '\n';
            repo.conflict("merge", {{"large.txt", {"base\n" + large, "main\n" + large, "side\n" + large}}});
            writeFile(repo.path("large.txt"), "preserved large manual result\n" + large);
            const auto before = repo.snapshot();
            const auto smallOther = readFile(repo.path("excluded.txt"));
            ScopedEnvironment environment(repo.environment);
            correspondence_faults::Failure fault;
            ApplicationController controller;
            openController(controller, repo);
            CHECK(correspondence_faults::failures == 1);
            CHECK(controller.files()->totalFiles() == 3 && controller.loadErrorFiles() == 1);
            const auto &failed = openedFile(controller, "large.txt");
            CHECK(failed.support() == Support::Unreadable && failed.snapshots() && !failed.correspondence());
            CHECK(failed.supportDetail().find("large.txt") != std::string::npos);
            CHECK(failed.supportDetail().find("injected large-file correspondence resource failure") != std::string::npos);
            CHECK(failed.current().draft.text == ("preserved large manual result\n" + large).toStdString());
            CHECK(failed.snapshots()->stage2->worktree.utf8 == ("main\n" + large).toStdString());
            CHECK(failed.snapshots()->stage3->worktree.utf8 == ("side\n" + large).toStdString());
            for (const auto path : {"tracked.txt", "excluded.txt"}) {
                const auto &small = openedFile(controller, path);
                CHECK(small.support() == Support::Supported && small.correspondence() && small.snapshots());
                CHECK(small.snapshots()->stage2->worktree.utf8 == "main\n" && small.snapshots()->stage3->worktree.utf8 == "side\n");
                CHECK(small.current().draft.text == (QByteArray(path) == "tracked.txt" ? repo.manual : smallOther).toStdString());
                CHECK(controller.selectFile(QString::fromStdString(small.id())));
                CHECK(controller.selectedFile().value("stage2").toMap().value("text").toString() == "main\n");
            }
            CHECK(!controller.files()->hasPendingRecovery() && !controller.files()->canFinish());
            CHECK(repo.snapshot() == before);
        });
        suite.run("TRUCE-0053 failed placeholder recovery keeps captured files visible blocks editing and retries the captured state", [] {
            Repository repo;
            repo.conflict("merge");
            const auto before = repo.snapshot();
            ScopedEnvironment environment(repo.environment);
            for (const bool failRecovery : {false, true}) {
                // Each case must lack an earlier usable recovery store. A valid
                // store from the healthy case would bypass the intended fault.
                QTemporaryDir recoveryData;
                auto isolatedEnvironment = repo.environment;
                isolatedEnvironment.insert("XDG_DATA_HOME", recoveryData.path());
                ScopedEnvironment isolated(isolatedEnvironment);
                ApplicationController controller;
                auto *model = controller.files();
                const auto location = model->recoveryDirectory();
                if (failRecovery) {
                    CHECK(QDir().mkpath(QFileInfo(location).path()));
                    writeFile(location, "block recovery storage with a regular file");
                }
                openController(controller, repo);
                CHECK(model->totalFiles() == 2 && model->hasPendingRecovery() == failRecovery);
                CHECK(controller.loadErrorFiles() == 0 && controller.outsideAppFiles() == 0);
                for (int row = 0; row < model->totalFiles(); ++row) {
                    CHECK(model->data(model->index(row), SessionModel::PendingInputRole).toBool() == failRecovery);
                    CHECK(model->data(model->index(row), SessionModel::RecoveryErrorRole).toString().isEmpty() == !failRecovery);
                }
                for (const auto path : {"tracked.txt", "excluded.txt"}) {
                    const auto &loaded = openedFile(controller, path);
                    CHECK(loaded.support() == Support::Supported && loaded.snapshots() && loaded.correspondence());
                    CHECK(loaded.supportDetail().empty());
                    CHECK(loaded.current().draft.text == readFile(repo.path(path)).toStdString());
                    CHECK(loaded.snapshots()->stage2->worktree.utf8 == "main\n" && loaded.snapshots()->stage3->worktree.utf8 == "side\n");
                    CHECK(controller.selectFile(QString::fromStdString(loaded.id())));
                    CHECK(controller.selectedFile().value("stage2").toMap().value("text").toString() == "main\n");
                    CHECK(controller.selectedFile().value("resultText").toString().toStdString() == loaded.current().draft.text);
                    if (failRecovery) {
                        CHECK(model->pendingDraft(loaded.id()) && *model->pendingDraft(loaded.id()) == loaded.current().draft);
                        CHECK(model->retryRecovery(loaded.id()) == Transition::Blocked);
                        CHECK(model->replaceDraft(loaded.id(), 1, {"must wait for recovery", {}, 0}) == Transition::Blocked);
                    }
                }
                if (failRecovery) {
                    CHECK(QFile::remove(location));
                    for (const auto path : {"tracked.txt", "excluded.txt"}) {
                        const auto id = openedFile(controller, path).id();
                        CHECK(model->retryRecovery(id) == Transition::Applied);
                    }
                }
                CHECK(!model->hasPendingRecovery());
                storage::RecoveryStore store(location);
                const auto recovered = store.read();
                CHECK(recovered.succeeded && recovered.transactions.size() == (failRecovery ? 2 : 4));
                // Successful placeholders may precede the two final captured-file records.
                for (qsizetype row = recovered.transactions.size() - 2; row < recovered.transactions.size(); ++row) {
                    const auto &state = recovered.transactions[row];
                    const auto sources = state.value("snapshots").toObject();
                    CHECK(!sources.isEmpty() && state.value("support").toInt() == static_cast<int>(Support::Supported));
                    QString error;
                    const auto encodedText = state.value("current").toObject().value("draft").toObject().value("text").toString().toLatin1();
                    CHECK(!store.payload(encodedText, &error).isEmpty() && error.isEmpty());
                }
                CHECK(model->replaceDraft(openedFile(controller).id(), 1, {"editable after recovery\n", {}, 0}) == Transition::Applied);
                CHECK(controller.startupState() == "ready" && repo.snapshot() == before);
            }
        });
        suite.run("merge and rebase captures retain verified sources provenance and the manual launch result", [] {
            for (const auto operation : {"merge", "rebase-merge", "rebase-apply"}) {
                Repository repo;
                repo.conflict(operation);
                const auto launchIdentity = identity(repo.path("tracked.txt"));
                ScopedEnvironment environment(repo.environment);
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(selected.support() == Support::Supported && selected.snapshots());
                const auto expected = *selected.snapshots();
                CHECK(expected.path == "tracked.txt" && expected.resultMode == "100644");
                CHECK(expected.launchResult.text.utf8 == repo.manual.toStdString());
                CHECK(expected.launchResult.text.bytes == repo.manual.toStdString());
                CHECK(selected.current().draft.text == expected.launchResult.text.utf8);
                CHECK(expected.launchResult.identity == launchIdentity);
                CHECK(expected.launchResult.text.format == SnapshotFormat{SnapshotFormat::Encoding::Utf8,
                    false, SnapshotFormat::Newline::Lf, true});
                const bool rebase = QString::fromLatin1(operation).startsWith("rebase");
                const auto &provenance = expected.provenance;
                CHECK(!provenance.sessionId.empty());
                CHECK(provenance.worktreeRoot == repo.root.toStdString());
                CHECK(provenance.gitDirectory == QFileInfo(repo.path(".git")).canonicalFilePath().toStdString());
                CHECK(provenance.operation == (rebase ? SnapshotProvenance::Operation::Rebase
                                                      : SnapshotProvenance::Operation::Merge));
                CHECK(provenance.stage2CommitId == repo.main.toStdString());
                CHECK(provenance.stage3CommitId == repo.side.toStdString());
                CHECK(provenance.baseCommitId == repo.git({"rev-parse", QString::fromLatin1(repo.side) + '^'}).trimmed().toStdString());
                if (rebase) {
                    CHECK(provenance.headName == "refs/heads/side");
                    CHECK(provenance.originalHead == repo.side.toStdString());
                    CHECK(provenance.onto == repo.main.toStdString());
                }
                CHECK(expected.earlier && expected.stage2 && expected.stage3);
                CHECK(expected.earlier->commitId == provenance.baseCommitId);
                CHECK(expected.stage2->commitId == provenance.stage2CommitId);
                CHECK(expected.stage3->commitId == provenance.stage3CommitId);
                CHECK(expected.earlier->label == "Common earlier version");
                CHECK(expected.stage2->label.starts_with(rebase ? "Current rebased state:" : "Current checkout:"));
                CHECK(expected.stage3->label.starts_with(rebase ? "Commit being replayed:" : "Merged commit:"));
                const std::array<const SourceSnapshot *, 3> sources{
                    &*expected.earlier, &*expected.stage2, &*expected.stage3};
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    const auto &source = *sources[i];
                    const auto object = repo.git({"rev-parse", ':' + QString::number(static_cast<unsigned int>(i + 1))
                        + ":tracked.txt"}).trimmed();
                    CHECK(source.objectId == object.toStdString() && source.mode == "100644");
                    CHECK(source.rawBytes == repo.git({"cat-file", "blob", QString::fromLatin1(object)}).toStdString());
                    CHECK(source.worktree.bytes == source.rawBytes && source.worktree.utf8 == source.rawBytes);
                    CHECK(source.worktree.format == SnapshotFormat{SnapshotFormat::Encoding::Utf8,
                        false, SnapshotFormat::Newline::Lf, true});
                }
                const auto &other = openedFile(controller, "excluded.txt");
                CHECK(other.snapshots() && other.snapshots()->provenance == provenance);
                auto *model = controller.files();
                auto edited = selected.current().draft;
                edited.text = "later manual edit\n";
                CHECK(model->replaceDraft(selected.id(), selected.current().id, edited) == Transition::Applied);
                CHECK(*selected.snapshots() == expected);
                CHECK(model->undo(selected.id(), selected.current().id) == Transition::Applied);
                CHECK(selected.current().draft.text == repo.manual.toStdString());
                CHECK(model->redo(selected.id(), selected.current().id) == Transition::Applied);
                writeFile(repo.path("tracked.txt"), "external replacement\n");
                repo.git({"add", "--", "tracked.txt"});
                writeFile(repo.path(".gitattributes"), "tracked.txt -text\n");
                CHECK(*selected.snapshots() == expected);
            }
        });
        suite.run("converted source and manual result captures retain independent bytes and format metadata", [] {
            struct FormatCase {
                QByteArray attributes;
                QByteArray earlier;
                QByteArray main;
                QByteArray side;
                QByteArray launch;
                SnapshotFormat::Encoding encoding;
                bool sourceBom;
                bool launchBom;
            };
            const FormatCase cases[]{
                {"text eol=crlf", "base\r\n", "main\r\n", "side\r\n", "\xef\xbb\xbfmanual \xc3\xa9\r\nkept",
                    SnapshotFormat::Encoding::Utf8, false, true},
                {"text eol=crlf working-tree-encoding=UTF-16",
                    QByteArray::fromHex("fffe62006100730065000d000a00"),
                    QByteArray::fromHex("fffe6d00610069006e000d000a00"),
                    QByteArray::fromHex("fffe73006900640065000d000a00"),
                    QByteArray::fromHex("fffe6d0061006e00750061006c002000e9000d000a006b00650070007400"),
                    SnapshotFormat::Encoding::Utf16LittleEndian, true, true},
                {"text eol=crlf working-tree-encoding=UTF-16BE",
                    QByteArray::fromHex("0062006100730065000d000a"),
                    QByteArray::fromHex("006d00610069006e000d000a"),
                    QByteArray::fromHex("0073006900640065000d000a"),
                    QByteArray::fromHex("006d0061006e00750061006c002000e9000d000a006b006500700074"),
                    SnapshotFormat::Encoding::Utf16BigEndian, false, false}
            };
            for (const auto &format : cases) {
                Repository repo;
                repo.git({"config", "core.autocrlf", "false"});
                repo.conflict("merge");
                writeFile(repo.path(".gitattributes"), "tracked.txt " + format.attributes + '\n');
                writeFile(repo.path("tracked.txt"), format.launch);
                const auto launchIdentity = identity(repo.path("tracked.txt"));
                ScopedEnvironment environment(repo.environment);
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(selected.support() == Support::Supported && selected.snapshots());
                const auto captured = *selected.snapshots();
                CHECK(captured.earlier && captured.stage2 && captured.stage3);
                const std::array<const SourceSnapshot *, 3> sources{
                    &*captured.earlier, &*captured.stage2, &*captured.stage3};
                const std::array<QByteArray, 3> checkout{format.earlier, format.main, format.side};
                const std::array<std::string, 3> canonical{"base\n", "main\n", "side\n"};
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    CHECK(sources[i]->rawBytes == canonical[i]);
                    CHECK(sources[i]->worktree.utf8 == canonical[i]);
                    CHECK(sources[i]->worktree.bytes == checkout[i].toStdString());
                    CHECK(sources[i]->worktree.bytes != sources[i]->rawBytes);
                    CHECK(sources[i]->worktree.format == SnapshotFormat{format.encoding,
                        format.sourceBom, SnapshotFormat::Newline::CrLf, true});
                }
                CHECK(selected.current().draft.text == "manual \xc3\xa9\nkept");
                CHECK(captured.launchResult.text.utf8 == selected.current().draft.text);
                CHECK(captured.launchResult.text.bytes == format.launch.toStdString());
                CHECK(captured.launchResult.text.format == SnapshotFormat{format.encoding,
                    format.launchBom, SnapshotFormat::Newline::CrLf, false});
                CHECK(captured.launchResult.identity == launchIdentity);
                CHECK(captured.conversion.attributes.at("text") == "set");
                CHECK(captured.conversion.attributes.at("eol") == "crlf");
                CHECK(captured.conversion.configuration.at("core.autocrlf") == "false");
                if (format.encoding == SnapshotFormat::Encoding::Utf8) {
                    CHECK(!captured.conversion.attributes.contains("working-tree-encoding"));
                } else {
                    CHECK(captured.conversion.attributes.at("working-tree-encoding")
                        == (format.encoding == SnapshotFormat::Encoding::Utf16LittleEndian ? "UTF-16" : "UTF-16BE"));
                }
                auto edited = selected.current().draft;
                edited.text += " later";
                CHECK(controller.files()->replaceDraft(selected.id(), selected.current().id, edited) == Transition::Applied);
                CHECK(*selected.snapshots() == captured);
            }
        });
        suite.run("add-add has an unavailable earlier source while an empty base remains captured", [] {
            for (const bool missing : {false, true}) {
                Repository repo;
                if (missing) repo.git({"rm", "--", "tracked.txt"});
                else {
                    writeFile(repo.path("tracked.txt"), {});
                    repo.git({"add", "--", "tracked.txt"});
                }
                repo.git({"commit", "--quiet", "-m", "prepare earlier source"});
                repo.conflict("merge");
                ScopedEnvironment environment(repo.environment);
                ApplicationController controller;
                openController(controller, repo);
                const auto &selected = openedFile(controller);
                CHECK(selected.support() == Support::Supported && selected.snapshots());
                const auto expected = *selected.snapshots();
                CHECK(expected.earlier.has_value() == !missing);
                CHECK(expected.stage2 && expected.stage3);
                CHECK(expected.stage2->rawBytes == "main\n" && expected.stage3->rawBytes == "side\n");
                CHECK(selected.current().draft.text == repo.manual.toStdString());
                if (!missing) {
                    CHECK(expected.earlier->rawBytes.empty());
                    CHECK(expected.earlier->worktree.bytes.empty() && expected.earlier->worktree.utf8.empty());
                    CHECK(!expected.earlier->objectId.empty() && expected.earlier->commitId.has_value());
                    CHECK(!expected.earlier->worktree.format.finalNewline);
                }
                auto edited = selected.current().draft;
                edited.text = "edited after capture\n";
                auto *model = controller.files();
                CHECK(model->replaceDraft(selected.id(), selected.current().id, edited) == Transition::Applied);
                CHECK(model->undo(selected.id(), selected.current().id) == Transition::Applied);
                CHECK(model->redo(selected.id(), selected.current().id) == Transition::Applied);
                CHECK(*selected.snapshots() == expected);
            }
        });
        suite.run("merge startup accepts an explicit root a subdirectory and the current directory", [&] {
            Repository repo;
            repo.conflict("merge");
            CHECK(QDir(repo.root).mkdir("nested"));
            for (const auto &arguments : QList<QStringList>{{repo.root}, {repo.path("nested")}, {}}) {
                const auto state = repo.launch(application, arguments, repo.path("nested"));
                ready(repo, state, "Merge");
                CHECK(state.value("stage2").toString().startsWith("Current checkout:"));
                CHECK(state.value("stage3").toString().startsWith("Merged commit:"));
                CHECK(file(state, "excluded.txt").value("support") == "supported");
            }
        });
        suite.run("both rebase startup routes preserve the result and truthful source identities", [&] {
            for (const auto operation : {"rebase-merge", "rebase-apply"}) {
                Repository repo;
                repo.conflict(operation);
                const auto state = repo.launch(application, {repo.root});
                ready(repo, state, "Rebase");
                CHECK(state.value("stage2").toString().startsWith("Current rebased state:"));
                CHECK(state.value("stage3").toString().startsWith("Commit being replayed:"));
            }
        });
        suite.run("unsupported cherry-pick keeps the inventory outside app without side labels", [&] {
            Repository repo;
            repo.conflict("cherry-pick");
            const auto state = repo.launch(application, {repo.root});
            CHECK(state.value("state") == "unsupported" && state.value("operation") == "Unsupported operation");
            CHECK(!state.value("detail").toString().isEmpty() && state.value("files").toArray().size() == 2);
            CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
            CHECK(state.value("resolved").toInt() == 0);
            for (const auto &value : state.value("files").toArray()) {
                CHECK(value.toObject().value("support") == "outside-app");
                CHECK(!value.toObject().value("detail").toString().isEmpty());
            }
        });
        suite.run("offline startup uses only local Git inspection with an unreachable remote", [&] {
            Repository repo;
            repo.conflict("merge");
            repo.offlineGuard();
            ready(repo, repo.launch(application, {repo.root}), "Merge");
            CHECK(!QFile::exists(repo.executables.path() + "/network-attempt"));
            const auto trace = readFile(repo.executables.path() + "/git-argv");
            CHECK(trace.contains("rev-parse\n") && trace.contains("cat-file\n"));
        });
        suite.run("excluded attributes retain the classifier reason beside an accepted manual UTF-8 result", [&] {
            Repository repo(true);
            repo.conflict("merge");
            const auto state = repo.launch(application, {repo.root});
            ready(repo, state, "Merge");
            const auto excluded = file(state, "excluded.txt");
            CHECK(excluded.value("support") == "outside-app" && excluded.value("text").toString().isEmpty());
            CHECK(excluded.value("detail").toString()
                  == "Outside app: stage 1 cannot be edited: Outside app: unsupported or contradictory text/eol/ident attributes");
            CHECK(state.value("detail").toString().contains("outside app remain outstanding"));
        });
        suite.run("a repository without an operation reports no session and no source identities", [&] {
            Repository repo;
            const auto state = repo.launch(application, {repo.root});
            CHECK(state.value("state") == "no-session" && state.value("files").toArray().isEmpty());
            CHECK(!state.value("detail").toString().isEmpty() && state.value("operation").toString().isEmpty());
            CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
        });
        suite.run("invalid selections expose an error without an empty supported session", [&] {
            Repository repo;
            for (const auto &selection : {repo.executables.path(), repo.path("missing")}) {
                const auto state = repo.launch(application, {selection});
                CHECK(state.value("state") == "error" && state.value("files").toArray().isEmpty());
                CHECK(!state.value("detail").toString().isEmpty());
                CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
            }
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) return 2;
    return StartupRouteTests{}.run(application.arguments()[1]);
}
