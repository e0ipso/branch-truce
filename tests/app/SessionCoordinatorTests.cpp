#include "app/SessionCoordinator.h"
#include "TestHarness.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

using namespace branch_truce;

namespace {
Draft draft(std::string text = "before\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}}, 0};
}

template<typename Predicate>
bool until(Predicate predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return predicate();
}

// Release barriers during unwinding too. Worker lambdas keep shared state,
// never references to a fixture or its model, even when an assertion fails.
struct Gate {
    struct State {
        std::atomic_bool entered{false};
        QSemaphore release;
        void pause()
        {
            entered = true;
            if (!release.tryAcquire(1, 5000)) throw std::runtime_error("worker barrier timed out");
        }
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    ~Gate() { open(); }
    void open() const { state->release.release(); }
    void wait() const { CHECK(until([&] { return state->entered.load(); })); }
};

struct DrainWorkers {
    ~DrainWorkers() { QThreadPool::globalInstance()->waitForDone(6000); }
};

TaskResult writeResult(const TaskInput &input, const TaskControl &control, const QByteArray &bytes)
{
    if (!control.beginMutation()) return {TaskStatus::Cancelled, MutationObservation::NotAttempted, {}, {}, {}};
    const auto path = QFile::encodeName(input.stamp.worktree) + '/' + QByteArray::fromStdString(input.path);
    const int fd = ::open(path.constData(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("fixture mutation open failed");
    const auto written = ::write(fd, bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const int closed = ::close(fd);
    if (written != bytes.size() || closed != 0) throw std::runtime_error("fixture mutation write failed");
    return {TaskStatus::Succeeded, MutationObservation::NotAttempted, bytes, {}, {}};
}

MutationObservation changed(const TaskInput &) { return MutationObservation::ObservedChanged; }

class Fixture {
public:
    Fixture()
    {
        CHECK(root.isValid());
        git({"init", "--quiet", "--initial-branch=main"});
        write("tracked.txt", "before\n");
        git({"add", "--", "tracked.txt"});
        coordinator = std::make_unique<SessionCoordinator>(model, "/usr/bin/git");
        QObject::connect(coordinator.get(), &SessionCoordinator::taskFinished, &model,
                         [this](const TaskCompletion &value) { finished.push_back(value); });
        replace();
    }
    ~Fixture()
    {
        coordinator.reset();
        for (const auto &path : rawFiles) ::unlink(path.constData());
    }
    void replace(const QByteArray &path = "tracked.txt", const QString &operation = "merge")
    {
        CHECK(coordinator->replaceSession(root.path(), operation,
              {FileSession("file", path.toStdString(), draft())}));
    }
    void write(const QByteArray &relative, const QByteArray &bytes)
    {
        const auto path = QFile::encodeName(root.path()) + '/' + relative;
        const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(fd >= 0);
        rawFiles.push_back(path);
        const auto written = ::write(fd, bytes.constData(), static_cast<std::size_t>(bytes.size()));
        const int closed = ::close(fd);
        CHECK(written == bytes.size() && closed == 0);
    }
    QByteArray git(const QStringList &arguments, const QByteArray &input = {}) const
    {
        QProcess process;
        QProcessEnvironment environment;
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_CONFIG_GLOBAL", "/dev/null");
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root.path());
        auto argv = QStringList{"--no-pager", "--literal-pathspecs"};
        argv.append(arguments);
        process.start("/usr/bin/git", argv);
        CHECK(process.waitForStarted(5000));
        CHECK(process.write(input) == input.size());
        process.closeWriteChannel();
        CHECK(process.waitForFinished(5000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    void wait(std::size_t count)
    {
        CHECK(until([&] { return finished.size() == count && coordinator->pendingTasks() == 0; }));
        QCoreApplication::processEvents();
        CHECK(finished.size() == count);
    }
    QTemporaryDir root{QStringLiteral("/tmp/truce-coordinator-XXXXXX")};
    SessionModel model;
    std::unique_ptr<SessionCoordinator> coordinator;
    std::vector<TaskCompletion> finished;
    QList<QByteArray> rawFiles;
};
}

class SessionCoordinatorTests {
public:
    int run()
    {
        test_support::Suite suite("SessionCoordinatorTests");
        const auto test = [&](const char *name, auto body) {
            suite.run(name, [&] { DrainWorkers drain; body(); });
        };
        test("background snapshots leave the event loop responsive and complete on its thread", [] {
            Fixture f;
            Gate gate;
            auto *guiThread = QThread::currentThread();
            auto workerThread = std::make_shared<std::atomic_bool>(false);
            int applied = 0;
            bool timerFired = false;
            const auto id = f.coordinator->submit("file", 1,
                [state = gate.state, workerThread, guiThread](const TaskInput &input, const TaskControl &control) {
                    *workerThread = QThread::currentThread() != guiThread && !control.beginMutation();
                    state->pause();
                    return TaskResult{TaskStatus::Succeeded, MutationObservation::NotAttempted,
                                      QByteArray::fromStdString(input.revision.draft.text), {}, {}};
                }, [&](const TaskCompletion &completion) {
                    CHECK(QThread::currentThread() == guiThread);
                    CHECK(completion.disposition == TaskDisposition::Current);
                    ++applied;
                });
            CHECK(id != 0 && f.coordinator->pendingTasks() == 1 && f.finished.empty() && applied == 0);
            gate.wait();
            QTimer::singleShot(0, &f.model, [&] { timerFired = true; });
            CHECK(until([&] { return timerFired; }));
            CHECK(workerThread->load() && f.finished.empty() && f.coordinator->pendingTasks() == 1);
            gate.open();
            f.wait(1);
            CHECK(applied == 1 && f.finished[0].result.data == "before\n");
            const auto &stamp = f.finished[0].stamp;
            CHECK(stamp.id == id && stamp.generation == f.coordinator->generation());
            CHECK(stamp.worktree == f.root.path() && stamp.operation == "merge" && stamp.fileId == "file");
            CHECK(stamp.inputRevision == 1 && stamp.resultRevision == 1 && stamp.decisionRevision == 1);
            CHECK(f.coordinator->mutationOutcomes().empty());
        });
        test("invalid requests never dispatch or emit a completion", [] {
            Fixture f;
            const auto work = [](const TaskInput &, const TaskControl &) { return TaskResult{}; };
            CHECK(f.coordinator->submit("missing", 1, work) == 0);
            CHECK(f.coordinator->submit("file", 2, work) == 0);
            CHECK(f.coordinator->submit("file", 1, {}) == 0);
            CHECK(f.coordinator->submitMutation("file", 1, work, {}) == 0);
            CHECK(!f.coordinator->cancel(999));
            CHECK(!f.coordinator->replaceSession(f.root.path(), "merge",
                  {FileSession("same", "one", draft()), FileSession("same", "two", draft())}));
            CHECK(!f.coordinator->replaceSession(f.root.path() + "/absent", "merge", {}));
            QCoreApplication::processEvents();
            CHECK(f.coordinator->generation() == 1 && f.coordinator->pendingTasks() == 0 && f.finished.empty());
        });
        test("result and decision edits fence late worker snapshots", [] {
            for (const bool decisionsOnly : {false, true}) {
                Fixture f;
                Gate gate;
                int applied = 0;
                CHECK(f.coordinator->submit("file", 1,
                    [state = gate.state](const TaskInput &input, const TaskControl &) {
                        state->pause();
                        return TaskResult{TaskStatus::Succeeded, MutationObservation::NotAttempted,
                                          QByteArray::fromStdString(input.revision.draft.text), {}, {}};
                    }, [&](const TaskCompletion &) { ++applied; }) != 0);
                gate.wait();
                auto edited = draft(decisionsOnly ? "before\n" : "edited\n");
                if (decisionsOnly) edited.decisions["left"] = {DecisionValue::Ignored, {}};
                CHECK(f.model.replaceDraft("file", 1, edited) == Transition::Applied);
                gate.open();
                f.wait(1);
                CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleRevision);
                CHECK(f.finished[0].result.data == "before\n" && f.model.file("file")->current().draft == edited);
            }
        });
        test("editing during mutation inspection retains evidence without applying it to the new revision", [] {
            Fixture f;
            Gate inspect;
            int applied = 0;
            const auto id = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "old result\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; },
                [&](const TaskCompletion &) { ++applied; });
            CHECK(id != 0);
            inspect.wait();
            CHECK(f.model.replaceDraft("file", 1, draft("new draft\n")) == Transition::Applied);
            inspect.open();
            f.wait(1);
            CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleRevision);
            CHECK(f.finished[0].result.status == TaskStatus::Succeeded);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged);
            CHECK(f.finished[0].stamp.inputRevision == 1 && f.finished[0].result.data == "old result\n");
            CHECK(f.model.file("file")->current().draft.text == "new draft\n");
            CHECK(f.coordinator->mutationOutcomes().size() == 1 && f.coordinator->mutationOutcomes()[0].stamp.id == id);
        });
        test("session replacement fences work even with the same file identity", [] {
            Fixture f;
            Gate gate;
            int applied = 0;
            CHECK(f.coordinator->submit("file", 1,
                [state = gate.state](const TaskInput &, const TaskControl &) {
                    state->pause();
                    return TaskResult{};
                }, [&](const TaskCompletion &) { ++applied; }) != 0);
            gate.wait();
            f.replace("tracked.txt", "rebase");
            CHECK(f.coordinator->generation() == 2 && f.coordinator->pendingTasks() == 1);
            gate.open();
            f.wait(1);
            CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[0].stamp.generation == 1 && f.finished[0].stamp.operation == "merge");
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled);
        });
        test("worker inspection and Git staging share a FIFO mutation queue", [] {
            Fixture f;
            Gate inspect;
            const auto original = f.git({"ls-files", "--stage", "-z"});
            const auto expected = f.git({"hash-object", "--stdin"}, "first\n").trimmed();
            const auto first = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "first\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; });
            const auto second = f.coordinator->submitGit("file", 1, {git::Operation::StageFile, {}, "tracked.txt", {}, 0, 0});
            const auto third = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "third\n"); }, changed);
            CHECK(first != 0 && second > first && third > second);
            inspect.wait();
            CHECK(f.coordinator->submit("file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; }) != 0);
            CHECK(until([&] { return f.finished.size() == 1; }));
            CHECK(f.finished[0].result.mutation == MutationObservation::NotAttempted);
            CHECK(f.git({"ls-files", "--stage", "-z"}) == original);
            inspect.open();
            f.wait(4);
            CHECK(f.finished[1].stamp.id == first && f.finished[2].stamp.id == second && f.finished[3].stamp.id == third);
            CHECK(f.finished[2].result.gitResult.has_value());
            CHECK(f.finished[2].result.gitResult->outcome == git::Outcome::Success);
            CHECK(f.finished[2].result.gitResult->indexAfter == "100644 " + expected + " 0\ttracked.txt" + QByteArray(1, '\0'));
            CHECK(f.git({"show", ":tracked.txt"}) == "first\n");
            QFile file(f.root.path() + "/tracked.txt");
            CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "third\n");
            CHECK(f.coordinator->mutationOutcomes().size() == 3);
        });
        test("cancellation before dispatch and while queued has no side effects", [] {
            Fixture f;
            Gate inspect;
            auto calls = std::make_shared<std::atomic_int>(0);
            const auto work = [calls](const TaskInput &, const TaskControl &) { ++*calls; return TaskResult{}; };
            int applied = 0;
            const auto beforeDispatch = f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; });
            CHECK(f.coordinator->cancel(beforeDispatch) && f.coordinator->cancel(beforeDispatch));
            f.wait(1);
            CHECK(f.finished[0].disposition == TaskDisposition::Cancelled && calls->load() == 0 && applied == 0);
            CHECK(!f.coordinator->cancel(beforeDispatch));
            CHECK(f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "first\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; }) != 0);
            inspect.wait();
            const auto queued = f.coordinator->submitMutation("file", 1, work, changed,
                                                            [&](const TaskCompletion &) { ++applied; });
            CHECK(f.coordinator->cancel(queued));
            CHECK(until([&] { return f.finished.size() == 2; }));
            CHECK(f.finished[1].stamp.id == queued && f.finished[1].result.mutation == MutationObservation::NotAttempted);
            CHECK(calls->load() == 0 && applied == 0);
            inspect.open();
            f.wait(3);
        });
        test("cancellation wins admission while a worker is preparing a mutation", [] {
            Fixture f;
            Gate preparation;
            auto inspected = std::make_shared<std::atomic_bool>(false);
            const auto before = f.git({"ls-files", "--stage", "-z"});
            const auto id = f.coordinator->submitMutation("file", 1,
                [state = preparation.state](const TaskInput &input, const TaskControl &control) {
                    state->pause();
                    return writeResult(input, control, "must not be written\n");
                }, [inspected](const TaskInput &) { *inspected = true; return MutationObservation::ObservedChanged; });
            preparation.wait();
            CHECK(f.coordinator->cancel(id));
            preparation.open();
            f.wait(1);
            CHECK(!inspected->load() && f.finished[0].result.mutation == MutationObservation::NotAttempted);
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled && f.finished[0].disposition == TaskDisposition::Cancelled);
            CHECK(f.git({"ls-files", "--stage", "-z"}) == before);
            QFile file(f.root.path() + "/tracked.txt");
            CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "before\n");
        });
        test("cancellation after mutation retains inspection and holds the next writer", [] {
            Fixture f;
            Gate inspect;
            int applied = 0;
            auto nextRan = std::make_shared<std::atomic_bool>(false);
            const auto id = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "first\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; },
                [&](const TaskCompletion &) { ++applied; });
            inspect.wait();
            CHECK(f.coordinator->cancel(id));
            CHECK(f.coordinator->submitMutation("file", 1,
                [nextRan](const TaskInput &input, const TaskControl &control) {
                    *nextRan = true;
                    return writeResult(input, control, "next\n");
                }, changed) != 0);
            QCoreApplication::processEvents();
            CHECK(!nextRan->load() && f.finished.empty() && f.coordinator->pendingTasks() == 2);
            inspect.open();
            f.wait(2);
            CHECK(applied == 0 && nextRan->load());
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled && f.finished[0].disposition == TaskDisposition::Cancelled);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged && f.finished[0].result.data == "first\n");
            CHECK(f.coordinator->mutationOutcomes()[0].stamp.id == id);
        });
        test("session replacement through a symlink retains an active worktree queue", [] {
            Fixture f;
            Gate inspect;
            QTemporaryDir aliases;
            CHECK(aliases.isValid() && QFile::link(f.root.path(), aliases.path() + "/alias"));
            auto nextRan = std::make_shared<std::atomic_bool>(false);
            const auto old = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "old\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; });
            inspect.wait();
            CHECK(f.coordinator->replaceSession(aliases.path() + "/alias", "next-merge", {FileSession("file", "tracked.txt", draft())}));
            const auto next = f.coordinator->submitMutation("file", 1,
                [nextRan](const TaskInput &input, const TaskControl &control) {
                    *nextRan = true;
                    return writeResult(input, control, "next\n");
                }, changed);
            CHECK(next != 0);
            QCoreApplication::processEvents();
            CHECK(!nextRan->load() && f.finished.empty());
            inspect.open();
            f.wait(2);
            CHECK(f.finished[0].stamp.id == old && f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged);
            CHECK(f.finished[1].stamp.id == next && f.finished[1].stamp.worktree == f.root.path());
            CHECK(f.finished[1].disposition == TaskDisposition::Current && nextRan->load());
        });
        test("worker and inspection failures retain honest mutation outcomes and release the queue", [] {
            for (int scenario = 0; scenario < 5; ++scenario) {
                Fixture f;
                int applied = 0;
                const auto id = f.coordinator->submitMutation("file", 1,
                    [scenario](const TaskInput &input, const TaskControl &control) {
                        if (scenario == 0) return TaskResult{};
                        auto result = writeResult(input, control, "written\n");
                        if (scenario == 1) throw std::runtime_error("worker failure after write");
                        return result;
                    }, [scenario](const TaskInput &) {
                        if (scenario == 2) throw std::runtime_error("inspection failure");
                        return scenario == 3 ? MutationObservation::NotAttempted
                            : scenario == 4 ? MutationObservation::Uncertain : MutationObservation::ObservedChanged;
                    }, [&](const TaskCompletion &value) { CHECK(value.result.status == TaskStatus::Failed); ++applied; });
                CHECK(id != 0);
                CHECK(f.coordinator->submitMutation("file", 1,
                    [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "retry\n"); }, changed) != 0);
                f.wait(2);
                CHECK(applied == 1 && f.finished[0].result.status == TaskStatus::Failed);
                CHECK(f.finished[0].result.mutation == (scenario == 0 ? MutationObservation::NotAttempted
                    : scenario == 1 ? MutationObservation::ObservedChanged : MutationObservation::Uncertain));
                CHECK(!f.finished[0].result.diagnostic.isEmpty() && f.finished[1].result.status == TaskStatus::Succeeded);
                CHECK(f.coordinator->mutationOutcomes().size() == 2);
            }
        });
        test("teardown returns while workers or inspection are blocked and suppresses callbacks", [] {
            for (const bool duringInspection : {false, true}) {
                Fixture f;
                Gate gate;
                auto cancelled = std::make_shared<std::atomic_bool>(false);
                int applied = 0;
                const auto work = [state = gate.state, cancelled, duringInspection](const TaskInput &input, const TaskControl &control) {
                    if (duringInspection) return writeResult(input, control, "written\n");
                    state->pause();
                    *cancelled = control.cancellationRequested();
                    return TaskResult{};
                };
                if (duringInspection) {
                    CHECK(f.coordinator->submitMutation("file", 1, work,
                        [state = gate.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; },
                        [&](const TaskCompletion &) { ++applied; }) != 0);
                } else {
                    CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; }) != 0);
                }
                gate.wait();
                QElapsedTimer timer;
                timer.start();
                f.coordinator.reset();
                CHECK(timer.elapsed() < 500);
                gate.open();
                CHECK(QThreadPool::globalInstance()->waitForDone(5000));
                QCoreApplication::processEvents();
                CHECK(f.finished.empty() && applied == 0 && (duringInspection || cancelled->load()));
            }
        });
        test("completion can replace a session and enqueue work before diagnostic notifications", [] {
            Fixture f;
            int applied = 0;
            bool replacementQueued = false;
            const auto work = [](const TaskInput &, const TaskControl &) { return TaskResult{}; };
            QObject::connect(f.coordinator.get(), &SessionCoordinator::taskFinished, &f.model,
                [&](const TaskCompletion &value) {
                    if (value.stamp.generation == 1) CHECK(applied == 1 && replacementQueued);
                });
            CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) {
                ++applied;
                f.replace("tracked.txt", "new-merge");
                CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; }) != 0);
                replacementQueued = true;
            }) != 0);
            f.wait(2);
            CHECK(applied == 2 && f.finished[0].disposition == TaskDisposition::Current);
            CHECK(f.finished[1].stamp.generation == 2);
        });
        test("pending notifications can cancel and replace without starting superseded work", [] {
            Fixture f;
            auto calls = std::make_shared<std::atomic_int>(0);
            bool replaced = false;
            bool resetGuarded = false;
            const auto work = [calls](const TaskInput &, const TaskControl &) { ++*calls; return TaskResult{}; };
            QObject::connect(&f.model, &QAbstractItemModel::modelAboutToBeReset, &f.model, [&] {
                resetGuarded = f.coordinator->submit("file", 1, work) == 0
                    && !f.coordinator->replaceSession(f.root.path(), "recursive", {});
            });
            QObject::connect(f.coordinator.get(), &SessionCoordinator::pendingTasksChanged, &f.model, [&] {
                if (replaced || f.coordinator->pendingTasks() != 1) return;
                replaced = true;
                f.replace("tracked.txt", "replacement");
                CHECK(f.coordinator->submit("file", 1, work) != 0);
            });
            CHECK(f.coordinator->submit("file", 1, work) != 0);
            f.wait(2);
            CHECK(replaced && resetGuarded && calls->load() == 1);
            CHECK(f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[1].disposition == TaskDisposition::Current);
        });
        test("callbacks and diagnostic listeners may destroy the coordinator", [] {
            for (const bool inCompletion : {false, true}) {
                Fixture f;
                bool destroyed = false;
                if (!inCompletion) {
                    QObject::connect(f.coordinator.get(), &SessionCoordinator::taskFinished, &f.model,
                        [&](const TaskCompletion &) { f.coordinator.reset(); destroyed = true; });
                }
                CHECK(f.coordinator->submit("file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; },
                    [&](const TaskCompletion &) { if (inCompletion) { f.coordinator.reset(); destroyed = true; } }) != 0);
                CHECK(until([&] { return destroyed; }));
                QCoreApplication::processEvents();
                CHECK(!f.coordinator && f.finished.size() == (inCompletion ? 0U : 1U));
            }
        });
        test("exact byte Git requests are admitted and handed off while different paths are rejected", [] {
            const QList<QByteArray> paths{"space\tand\nnewline", ":(glob)*", "--all", QByteArray("odd-") + char(0xff),
                                         QByteArray("odd-") + char(0xfe), QByteArray::fromHex("636166c3a9")};
            for (const auto &path : paths) {
                Fixture f;
                f.write(path, "edited\n");
                const auto beforeObject = f.git({"hash-object", "-w", "--stdin"}, "before\n").trimmed();
                f.git({"update-index", "-z", "--index-info"}, "100644 " + beforeObject + '\t' + path + '\0');
                const auto indexBefore = f.git({"ls-files", "--stage", "-z"});
                const auto expected = f.git({"hash-object", "--stdin"}, "edited\n").trimmed();
                f.replace(path);
                int applied = 0;
                git::Request request{git::Operation::StageFile, "/not-the-session", path + "different", {}, 999, 999};
                CHECK(f.coordinator->submitGit("file", 1, request) == 0);
                request.path = QString::fromUtf8(path).toUtf8();
                if (request.path != path) CHECK(f.coordinator->submitGit("file", 1, request) == 0);
                QCoreApplication::processEvents();
                CHECK(f.finished.empty() && f.coordinator->pendingTasks() == 0);
                CHECK(f.git({"ls-files", "--stage", "-z"}) == indexBefore);
                request.path = path;
                const auto id = f.coordinator->submitGit("file", 1, request, [&](const TaskCompletion &) { ++applied; });
                CHECK(id != 0);
                f.wait(1);
                CHECK(applied == 1 && f.finished[0].stamp.fileId == "file");
                CHECK(f.finished[0].result.gitResult.has_value());
                const auto &result = *f.finished[0].result.gitResult;
                CHECK(result.outcome == git::Outcome::Success && result.mutation == git::Mutation::ObservedChanged);
                CHECK(result.request.path == path && result.request.directory == f.root.path());
                CHECK(result.request.sessionGeneration == f.coordinator->generation() && result.request.inputRevision == 1);
                CHECK(result.indexAfter == "100644 " + expected + " 0\t" + path + QByteArray(1, '\0'));
                CHECK(f.coordinator->mutationOutcomes().size() == 1);
            }
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::cout.setf(std::ios::unitbuf);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    return SessionCoordinatorTests{}.run();
}
