#include "TestHarness.h"
#include "TestSession.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/Translations.h"
#include "fixtures/Await.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/EventLoop.h"
#include "fixtures/Files.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/GitRepository.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/ScriptedGit.h"
#include "recovery/RecoveryRestore.h"
#include "session/SessionCoordinator.h"
#include "storage/OffThread.h"
#include <QCoreApplication>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

using test_support::await;

using namespace branch_truce;

namespace {
Draft draft(std::string text = "before\n") { return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}}, 0}; }

using test_support::waitUntil;

// Release barriers during unwinding too. Worker lambdas keep shared state,
// never references to a fixture or its model, even when an assertion fails.
struct Gate {
    // Where the worker is: not at the gate yet, waiting at it, let through, or given up on it.
    enum class Phase { Before, Waiting, Released, TimedOut };
    struct State {
        std::atomic_bool entered{false};
        std::atomic<Phase> phase{Phase::Before};
        QSemaphore release;
        void pause()
        {
            phase = Phase::Waiting;
            entered = true;
            if (!release.tryAcquire(1, 5000)) {
                phase = Phase::TimedOut;
                throw std::runtime_error("worker barrier timed out");
            }
            phase = Phase::Released;
        }
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    ~Gate() { open(); }
    void open() const { state->release.release(); }
    void wait() const
    {
        waitUntil([&] { return state->entered.load(); }, "the worker did not reach the gate");
    }
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
        write("tracked.txt", "before\n");
        git({"add", "--", "tracked.txt"});
        coordinator = std::make_unique<SessionCoordinator>(
            model.store, git::gitAdapters({test_support::gitExecutable(), git::GitEnvironment::fromProcess()}),
            workers);
        QObject::connect(coordinator.get(), &SessionCoordinator::taskFinished, &model.store,
                         [this](const TaskCompletion &value) { finished.push_back(value); });
        replace();
    }
    ~Fixture() { coordinator.reset(); }
    QString root() const { return repository.root(); }
    void replace(const QByteArray &path = "tracked.txt", const QString &operation = "merge")
    {
        CHECK(coordinator->replaceSession(root(), operation, {FileSession("file", path.toStdString(), draft())}));
        test_support::awaitRecovery(model.store);
    }
    // Writes a worktree file named by raw bytes.
    void write(const QByteArray &relative, const QByteArray &bytes) { repository.writePath(relative, bytes); }
    QByteArray git(const QStringList &arguments, const QByteArray &input = {}) const
    {
        return repository.git(arguments, input);
    }
    void wait(std::size_t count)
    {
        waitUntil([&] { return finished.size() == count && coordinator->pendingTasks() == 0; },
                  "the coordinator did not finish " + std::to_string(count) + " tasks");
        QCoreApplication::processEvents();
        CHECK(finished.size() == count);
    }
    test_support::GitRepository repository;
    test_support::TestSession model;
    QThreadPool workers;
    std::unique_ptr<SessionCoordinator> coordinator;
    std::vector<TaskCompletion> finished;
};
}

class SessionCoordinatorTests {
public:
    int run()
    {
        test_support::Suite suite("SessionCoordinatorTests");
        const auto test = [&](const char *name, auto body) { suite.run(name, [&] { body(); }); };
        test("background snapshots leave the event loop responsive and complete on its thread", [] {
            Fixture f;
            Gate gate;
            auto *guiThread = QThread::currentThread();
            auto workerThread = std::make_shared<std::atomic_bool>(false);
            int applied = 0;
            bool timerFired = false;
            const auto id = f.coordinator->submit(
                "file", 1,
                [state = gate.state, workerThread, guiThread](const TaskInput &input, const TaskControl &control) {
                    *workerThread = QThread::currentThread() != guiThread && !control.beginMutation();
                    state->pause();
                    return TaskResult{TaskStatus::Succeeded,
                                      MutationObservation::NotAttempted,
                                      QByteArray::fromStdString(input.revision.draft.text),
                                      {},
                                      {}};
                },
                [&](const TaskCompletion &completion) {
                    CHECK(QThread::currentThread() == guiThread);
                    CHECK(completion.disposition == TaskDisposition::Current);
                    ++applied;
                });
            CHECK(id != 0 && f.coordinator->pendingTasks() == 1 && f.finished.empty() && applied == 0);
            gate.wait();
            QTimer::singleShot(0, &f.model.store, [&] { timerFired = true; });
            waitUntil([&] { return timerFired; }, "the event loop did not run while the worker waited");
            CHECK(workerThread->load() && f.finished.empty() && f.coordinator->pendingTasks() == 1);
            gate.open();
            f.wait(1);
            CHECK(applied == 1 && f.finished[0].result.data == "before\n");
            const auto &stamp = f.finished[0].stamp;
            CHECK(stamp.id == id && stamp.generation == f.coordinator->generation());
            CHECK(stamp.worktree == f.root() && stamp.operation == "merge" && stamp.fileId == "file");
            CHECK(stamp.inputRevision == 1 && stamp.resultRevision == 1 && stamp.decisionRevision == 1);
        });
        test("invalid requests never dispatch or emit a completion", [] {
            Fixture f;
            const auto work = [](const TaskInput &, const TaskControl &) { return TaskResult{}; };
            CHECK(f.coordinator->submit("missing", 1, work) == 0);
            CHECK(f.coordinator->submit("file", 2, work) == 0);
            CHECK(f.coordinator->submit("file", 1, {}) == 0);
            CHECK(f.coordinator->submitMutation("file", 1, work, {}) == 0);
            CHECK(!f.coordinator->cancel(999));
            CHECK(!f.coordinator->replaceSession(
                f.root(), "merge", {FileSession("same", "one", draft()), FileSession("same", "two", draft())}));
            CHECK(!f.coordinator->replaceSession(f.root() + "/absent", "merge", {}));
            QCoreApplication::processEvents();
            CHECK(f.coordinator->generation() == 1 && f.coordinator->pendingTasks() == 0 && f.finished.empty());
        });
        test("result and decision edits fence late worker snapshots", [] {
            for (const bool decisionsOnly : {false, true}) {
                Fixture f;
                Gate gate;
                int applied = 0;
                CHECK(f.coordinator->submit(
                          "file", 1,
                          [state = gate.state](const TaskInput &input, const TaskControl &) {
                              state->pause();
                              return TaskResult{TaskStatus::Succeeded,
                                                MutationObservation::NotAttempted,
                                                QByteArray::fromStdString(input.revision.draft.text),
                                                {},
                                                {}};
                          },
                          [&](const TaskCompletion &) { ++applied; })
                      != 0);
                gate.wait();
                auto edited = draft(decisionsOnly ? "before\n" : "edited\n");
                if (decisionsOnly) edited.decisions["left"] = {DecisionValue::Ignored, {}};
                CHECK(await(f.model.store.replaceDraft("file", 1, edited)) == Transition::Applied);
                gate.open();
                f.wait(1);
                CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleRevision);
                CHECK(f.finished[0].result.data == "before\n" && f.model.store.file("file")->current().draft == edited);
            }
        });
        test("editing during mutation inspection retains evidence without applying it to the new revision", [] {
            Fixture f;
            Gate inspect;
            int applied = 0;
            const auto id = f.coordinator->submitMutation(
                "file", 1,
                [](const TaskInput &input, const TaskControl &control) {
                    return writeResult(input, control, "old result\n");
                },
                [state = inspect.state](const TaskInput &) {
                    state->pause();
                    return MutationObservation::ObservedChanged;
                },
                [&](const TaskCompletion &) { ++applied; });
            CHECK(id != 0);
            inspect.wait();
            CHECK(await(f.model.store.replaceDraft("file", 1, draft("new draft\n"))) == Transition::Applied);
            inspect.open();
            f.wait(1);
            CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleRevision);
            CHECK(f.finished[0].result.status == TaskStatus::Succeeded);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged);
            CHECK(f.finished[0].stamp.inputRevision == 1 && f.finished[0].result.data == "old result\n");
            CHECK(f.model.store.file("file")->current().draft.text == "new draft\n");
            CHECK(f.finished[0].stamp.id == id);
        });
        test("closing waits for an admitted mutation and records what it observed", [] {
            Fixture f;
            Gate inspect;
            CHECK(f.coordinator->submitMutation(
                      "file", 1,
                      [](const TaskInput &input, const TaskControl &control) {
                          return writeResult(input, control, "closed\n");
                      },
                      [state = inspect.state](const TaskInput &) {
                          state->pause();
                          return MutationObservation::ObservedChanged;
                      })
                  != 0);
            inspect.wait();
            // No event runs between here and the coordinator's end, so only the
            // wait on closing can record the observation.
            inspect.open();
            f.coordinator.reset();
            const auto replay = replayRecovery(storage::RecoveryStore(f.model.store.recoveryDirectory()));
            CHECK(replay.succeeded && replay.mutations.size() == 1);
            CHECK(replay.mutations.front().observation == MutationObservation::ObservedChanged);
        });
        test("session replacement fences work even with the same file identity", [] {
            Fixture f;
            Gate gate;
            int applied = 0;
            CHECK(f.coordinator->submit(
                      "file", 1,
                      [state = gate.state](const TaskInput &, const TaskControl &) {
                          state->pause();
                          return TaskResult{};
                      },
                      [&](const TaskCompletion &) { ++applied; })
                  != 0);
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
            const auto first = f.coordinator->submitMutation(
                "file", 1,
                [](const TaskInput &input, const TaskControl &control) {
                    return writeResult(input, control, "first\n");
                },
                [state = inspect.state](const TaskInput &) {
                    state->pause();
                    return MutationObservation::ObservedChanged;
                });
            const auto second =
                f.coordinator->submitGit("file", 1, {git::Operation::StageFile, {}, "tracked.txt", {}, 0, 0});
            const auto third = f.coordinator->submitMutation(
                "file", 1,
                [](const TaskInput &input, const TaskControl &control) {
                    return writeResult(input, control, "third\n");
                },
                changed);
            CHECK(first != 0 && second > first && third > second);
            inspect.wait();
            CHECK(f.coordinator->submit("file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; })
                  != 0);
            waitUntil([&] { return f.finished.size() == 1; }, "the first task did not finish");
            CHECK(f.finished[0].result.mutation == MutationObservation::NotAttempted);
            CHECK(f.git({"ls-files", "--stage", "-z"}) == original);
            inspect.open();
            f.wait(4);
            CHECK(f.finished[1].stamp.id == first && f.finished[2].stamp.id == second
                  && f.finished[3].stamp.id == third);
            CHECK(f.finished[2].result.gitResult.has_value());
            CHECK(f.finished[2].result.gitResult->outcome == git::Outcome::Success);
            CHECK(f.finished[2].result.gitResult->indexAfter
                  == "100644 " + expected + " 0\ttracked.txt" + QByteArray(1, '\0'));
            CHECK(f.git({"show", ":tracked.txt"}) == "first\n");
            QFile file(f.root() + "/tracked.txt");
            CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "third\n");
        });
        test("cancellation before dispatch and while queued has no side effects", [] {
            Fixture f;
            Gate inspect;
            auto calls = std::make_shared<std::atomic_int>(0);
            const auto work = [calls](const TaskInput &, const TaskControl &) {
                ++*calls;
                return TaskResult{};
            };
            int applied = 0;
            const auto beforeDispatch =
                f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; });
            CHECK(f.coordinator->cancel(beforeDispatch) && f.coordinator->cancel(beforeDispatch));
            f.wait(1);
            CHECK(f.finished[0].disposition == TaskDisposition::Cancelled && calls->load() == 0 && applied == 0);
            CHECK(!f.coordinator->cancel(beforeDispatch));
            CHECK(f.coordinator->submitMutation(
                      "file", 1,
                      [](const TaskInput &input, const TaskControl &control) {
                          return writeResult(input, control, "first\n");
                      },
                      [state = inspect.state](const TaskInput &) {
                          state->pause();
                          return MutationObservation::ObservedChanged;
                      })
                  != 0);
            inspect.wait();
            const auto queued =
                f.coordinator->submitMutation("file", 1, work, changed, [&](const TaskCompletion &) { ++applied; });
            CHECK(f.coordinator->cancel(queued));
            waitUntil([&] { return f.finished.size() == 2; }, "the second task did not finish");
            CHECK(f.finished[1].stamp.id == queued
                  && f.finished[1].result.mutation == MutationObservation::NotAttempted);
            CHECK(calls->load() == 0 && applied == 0);
            inspect.open();
            f.wait(3);
        });
        test("cancellation wins admission while a worker is preparing a mutation", [] {
            Fixture f;
            Gate preparation;
            auto inspected = std::make_shared<std::atomic_bool>(false);
            const auto before = f.git({"ls-files", "--stage", "-z"});
            const auto id = f.coordinator->submitMutation(
                "file", 1,
                [state = preparation.state](const TaskInput &input, const TaskControl &control) {
                    state->pause();
                    return writeResult(input, control, "must not be written\n");
                },
                [inspected](const TaskInput &) {
                    *inspected = true;
                    return MutationObservation::ObservedChanged;
                });
            preparation.wait();
            CHECK(f.coordinator->cancel(id));
            preparation.open();
            f.wait(1);
            CHECK(!inspected->load() && f.finished[0].result.mutation == MutationObservation::NotAttempted);
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled
                  && f.finished[0].disposition == TaskDisposition::Cancelled);
            CHECK(f.git({"ls-files", "--stage", "-z"}) == before);
            QFile file(f.root() + "/tracked.txt");
            CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "before\n");
        });
        test("cancellation after mutation retains inspection and holds the next writer", [] {
            Fixture f;
            Gate inspect;
            int applied = 0;
            auto nextRan = std::make_shared<std::atomic_bool>(false);
            const auto id = f.coordinator->submitMutation(
                "file", 1,
                [](const TaskInput &input, const TaskControl &control) {
                    return writeResult(input, control, "first\n");
                },
                [state = inspect.state](const TaskInput &) {
                    state->pause();
                    return MutationObservation::ObservedChanged;
                },
                [&](const TaskCompletion &) { ++applied; });
            inspect.wait();
            CHECK(f.coordinator->cancel(id));
            CHECK(f.coordinator->submitMutation(
                      "file", 1,
                      [nextRan](const TaskInput &input, const TaskControl &control) {
                          *nextRan = true;
                          return writeResult(input, control, "next\n");
                      },
                      changed)
                  != 0);
            QCoreApplication::processEvents();
            CHECK(!nextRan->load() && f.finished.empty() && f.coordinator->pendingTasks() == 2);
            inspect.open();
            f.wait(2);
            CHECK(applied == 0 && nextRan->load());
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled
                  && f.finished[0].disposition == TaskDisposition::Cancelled);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged
                  && f.finished[0].result.data == "first\n");
            CHECK(f.finished[0].stamp.id == id);
        });
        test("a mutation records its observation under the lease its intent was committed with", [] {
            Fixture f;
            Gate inspect;
            const auto id = f.coordinator->submitMutation(
                "file", 1,
                [](const TaskInput &input, const TaskControl &control) {
                    return writeResult(input, control, "first\n");
                },
                [state = inspect.state](const TaskInput &) {
                    state->pause();
                    return MutationObservation::ObservedChanged;
                });
            CHECK(id != 0);
            inspect.wait();
            const auto original = f.model.store.recoveryDirectory();
            // The session moves to a new store while the mutation is still inspecting.
            CHECK(f.model.store.startNewRecoveryStore() && f.model.store.recoveryDirectory() != original);
            CHECK(storage::RecoveryLease::acquire(original) == nullptr);
            inspect.open();
            f.wait(1);
            const auto replay = replayRecovery(storage::RecoveryStore(original));
            CHECK(replay.succeeded && replay.mutations.size() == 1);
            CHECK(replay.mutations.front().kind == MutationKind::Publication);
            CHECK(replay.mutations.front().observation == MutationObservation::ObservedChanged);
            // Once the observation is recorded the old store is free again.
            CHECK(storage::RecoveryLease::acquire(original) != nullptr);
        });
        test("session replacement through a symlink retains an active worktree queue", [] {
            Fixture f;
            Gate inspect;
            QTemporaryDir aliases;
            CHECK(aliases.isValid() && QFile::link(f.root(), aliases.path() + "/alias"));
            auto nextRan = std::make_shared<std::atomic_bool>(false);
            const auto old = f.coordinator->submitMutation(
                "file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "old\n"); },
                [state = inspect.state](const TaskInput &) {
                    state->pause();
                    return MutationObservation::ObservedChanged;
                });
            inspect.wait();
            CHECK(f.coordinator->replaceSession(aliases.path() + "/alias", "next-merge",
                                                {FileSession("file", "tracked.txt", draft())}));
            const auto next = f.coordinator->submitMutation(
                "file", 1,
                [nextRan](const TaskInput &input, const TaskControl &control) {
                    *nextRan = true;
                    return writeResult(input, control, "next\n");
                },
                changed);
            CHECK(next != 0);
            QCoreApplication::processEvents();
            CHECK(!nextRan->load() && f.finished.empty());
            inspect.open();
            f.wait(2);
            CHECK(f.finished[0].stamp.id == old && f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged);
            CHECK(f.finished[1].stamp.id == next && f.finished[1].stamp.worktree == f.root());
            CHECK(f.finished[1].disposition == TaskDisposition::Current && nextRan->load());
        });
        test("worker and inspection failures retain honest mutation outcomes and release the queue", [] {
            for (int scenario = 0; scenario < 5; ++scenario) {
                Fixture f;
                int applied = 0;
                const auto id = f.coordinator->submitMutation(
                    "file", 1,
                    [scenario](const TaskInput &input, const TaskControl &control) {
                        if (scenario == 0) return TaskResult{};
                        auto result = writeResult(input, control, "written\n");
                        if (scenario == 1) throw std::runtime_error("worker failure after write");
                        return result;
                    },
                    [scenario](const TaskInput &) {
                        if (scenario == 2) throw std::runtime_error("inspection failure");
                        return scenario == 3 ? MutationObservation::NotAttempted
                            : scenario == 4  ? MutationObservation::Uncertain
                                             : MutationObservation::ObservedChanged;
                    },
                    [&](const TaskCompletion &value) {
                        CHECK(value.result.status == TaskStatus::Failed);
                        ++applied;
                    });
                CHECK(id != 0);
                CHECK(f.coordinator->submitMutation(
                          "file", 1,
                          [](const TaskInput &input, const TaskControl &control) {
                              return writeResult(input, control, "retry\n");
                          },
                          changed)
                      != 0);
                f.wait(2);
                CHECK(applied == 1 && f.finished[0].result.status == TaskStatus::Failed);
                CHECK(f.finished[0].result.mutation
                      == (scenario == 0       ? MutationObservation::NotAttempted
                              : scenario == 1 ? MutationObservation::ObservedChanged
                                              : MutationObservation::Uncertain));
                CHECK(!f.finished[0].result.problem.isEmpty() && f.finished[1].result.status == TaskStatus::Succeeded);
            }
        });
        test("teardown drains owned work and inspection without publishing callbacks", [] {
            for (const bool duringInspection : {false, true}) {
                Fixture f;
                Gate gate;
                auto cancelled = std::make_shared<std::atomic_bool>(false);
                int applied = 0;
                const auto work = [state = gate.state, cancelled, duringInspection](const TaskInput &input,
                                                                                    const TaskControl &control) {
                    if (duringInspection) return writeResult(input, control, "written\n");
                    state->pause();
                    *cancelled = control.cancellationRequested();
                    return TaskResult{};
                };
                if (duringInspection) {
                    CHECK(f.coordinator->submitMutation(
                              "file", 1, work,
                              [state = gate.state](const TaskInput &) {
                                  state->pause();
                                  return MutationObservation::ObservedChanged;
                              },
                              [&](const TaskCompletion &) { ++applied; })
                          != 0);
                } else {
                    CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; }) != 0);
                }
                gate.wait();
                f.coordinator->cancelAll();
                gate.open();
                f.coordinator.reset();
                CHECK(gate.state->phase.load() == Gate::Phase::Released);
                if (duringInspection) {
                    const auto journal = storage::RecoveryStore(f.model.store.recoveryDirectory()).read();
                    CHECK(journal.succeeded
                          && journal.transactions.back().state.value("action") == "mutation-observation");
                }
                QCoreApplication::processEvents();
                CHECK(f.finished.empty() && applied == 0 && (duringInspection || cancelled->load()));
            }
        });
        test("teardown leaves unrelated work on the supplied pool running", [] {
            Fixture f;
            Gate unrelated;
            auto future = runOffThread(
                f.workers, &f.model.store,
                [state = unrelated.state] {
                    state->pause();
                    return true;
                },
                [](bool) {});
            unrelated.wait();
            f.coordinator.reset();
            CHECK(unrelated.state->phase == Gate::Phase::Waiting);
            unrelated.open();
            CHECK(await(future));
        });
        test("completion can replace a session and enqueue work before diagnostic notifications", [] {
            Fixture f;
            int applied = 0;
            bool replacementQueued = false;
            const auto work = [](const TaskInput &, const TaskControl &) { return TaskResult{}; };
            QObject::connect(f.coordinator.get(), &SessionCoordinator::taskFinished, &f.model.store,
                             [&](const TaskCompletion &value) {
                                 if (value.stamp.generation == 1) EXPECT(applied == 1 && replacementQueued);
                             });
            CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) {
                ++applied;
                EXPECT(f.coordinator->replaceSession(f.root(), "new-merge",
                                                     {FileSession("file", "tracked.txt", draft())}));
                EXPECT(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; }) != 0);
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
            const auto work = [calls](const TaskInput &, const TaskControl &) {
                ++*calls;
                return TaskResult{};
            };
            QObject::connect(&f.model.rows, &QAbstractItemModel::modelAboutToBeReset, &f.model.store, [&] {
                resetGuarded = f.coordinator->submit("file", 1, work) == 0
                    && !f.coordinator->replaceSession(f.root(), "recursive", {});
            });
            QObject::connect(f.coordinator.get(), &SessionCoordinator::pendingTasksChanged, &f.model.store, [&] {
                if (replaced || f.coordinator->pendingTasks() != 1) return;
                replaced = true;
                EXPECT(f.coordinator->replaceSession(f.root(), "replacement",
                                                     {FileSession("file", "tracked.txt", draft())}));
                EXPECT(f.coordinator->submit("file", 1, work) != 0);
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
                    QObject::connect(f.coordinator.get(), &SessionCoordinator::taskFinished, &f.model.store,
                                     [&](const TaskCompletion &) {
                                         f.coordinator.reset();
                                         destroyed = true;
                                     });
                }
                CHECK(f.coordinator->submit(
                          "file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; },
                          [&](const TaskCompletion &) {
                              if (inCompletion) {
                                  f.coordinator.reset();
                                  destroyed = true;
                              }
                          })
                      != 0);
                waitUntil([&] { return destroyed; }, "the coordinator was not destroyed");
                QCoreApplication::processEvents();
                CHECK(!f.coordinator && f.finished.size() == (inCompletion ? 0U : 1U));
            }
        });
        test("exact byte Git requests are admitted and handed off while different paths are rejected", [] {
            const QList<QByteArray> paths{"space\tand\nnewline",
                                          ":(glob)*",
                                          "--all",
                                          QByteArray("odd-") + char(0xff),
                                          QByteArray("odd-") + char(0xfe),
                                          QByteArray::fromHex("636166c3a9")};
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
                const auto id =
                    f.coordinator->submitGit("file", 1, request, [&](const TaskCompletion &) { ++applied; });
                CHECK(id != 0);
                f.wait(1);
                CHECK(applied == 1 && f.finished[0].stamp.fileId == "file");
                CHECK(f.finished[0].result.gitResult.has_value());
                const auto &result = *f.finished[0].result.gitResult;
                CHECK(result.outcome == git::Outcome::Success && result.mutation == git::Mutation::ObservedChanged);
                CHECK(result.request.path == path && result.request.directory == f.root());
                CHECK(result.request.sessionGeneration == f.coordinator->generation()
                      && result.request.inputRevision == 1);
                CHECK(result.indexAfter == "100644 " + expected + " 0\t" + path + QByteArray(1, '\0'));
            }
        });
        test("a Git task that times out fails with an uncertain mutation and releases the worktree queue", [] {
            test_support::GitRepository repository;
            test_support::writeFile(repository.path("tracked.txt"), "before\n");
            repository.git({"add", "--", "tracked.txt"});
            test_support::TestSession model;
            int answered = 0;
            const auto timedOut = [&answered](const git::Request &request) -> std::optional<git::Result> {
                if (request.operation != git::Operation::StageFile) return std::nullopt;
                ++answered;
                git::Result result;
                result.outcome = git::Outcome::TimedOut;
                result.mutation = git::Mutation::Uncertain;
                result.problem = {ProblemCode::GitDeadline, {QStringLiteral("30")}};
                return result;
            };
            QThreadPool workers;
            SessionCoordinator coordinator(
                model.store,
                test_support::ScriptedGit::runners(
                    {test_support::gitExecutable(), git::GitEnvironment::from(repository.environment())}, timedOut),
                workers);
            std::vector<TaskCompletion> finished;
            QObject::connect(&coordinator, &SessionCoordinator::taskFinished, &model.store,
                             [&](const TaskCompletion &value) { finished.push_back(value); });
            CHECK(
                coordinator.replaceSession(repository.root(), "merge", {FileSession("file", "tracked.txt", draft())}));
            const auto indexBefore = repository.index();
            git::Request stage{
                git::Operation::StageFile, repository.root(), "tracked.txt", {}, coordinator.generation(), 1};
            int applied = 0;
            CHECK(coordinator.submitGit("file", 1, stage, [&](const TaskCompletion &) { ++applied; }) != 0);
            // A worker mutation queued behind it runs once the Git task's slot is released.
            CHECK(coordinator.submitMutation(
                      "file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; }, changed)
                  != 0);
            // The answer comes from the event loop, never inside submitGit.
            CHECK(answered == 0 && finished.empty());
            waitUntil([&] { return finished.size() == 2 && coordinator.pendingTasks() == 0; },
                      "the timed-out task and the one queued behind it did not finish");
            CHECK(answered == 1 && applied == 1);
            const auto &timed = finished.front();
            CHECK(timed.result.status == TaskStatus::Failed);
            CHECK(timed.result.mutation == MutationObservation::Uncertain);
            CHECK(timed.result.gitResult && timed.result.gitResult->outcome == git::Outcome::TimedOut);
            CHECK(timed.result.gitResult->request.path == "tracked.txt");
            CHECK(finished.back().result.status == TaskStatus::Succeeded);
            CHECK(repository.index() == indexBefore);
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    std::cout.setf(std::ios::unitbuf);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    return SessionCoordinatorTests{}.run();
}
