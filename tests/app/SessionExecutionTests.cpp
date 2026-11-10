// Candidate execution, terminal delivery and admission exercise the production worker pipeline.
#include "TestHarness.h"
#include "app/Application.h"
#include "core/ExactLineCorrespondence.h"
#include "core/Utf8.h"
#include "fixtures/Await.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/RecoveryJournal.h"
#include "fixtures/SyncFaults.h"
#include "recovery/RecoveryRestore.h"
#include "session/SessionStore.h"
#include "storage/OffThread.h"
#include "storage/TextFormat.h"
#include <QCoreApplication>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>

using namespace branch_truce;
using test_support::await;
using test_support::waitUntil;

namespace {
struct Gate {
    std::atomic_bool armed{false}, entered{false};
    QSemaphore release;
    void hold()
    {
        if (!armed.exchange(false)) return;
        entered = true;
        if (!release.tryAcquire(1, 5000)) throw std::runtime_error("execution gate timed out");
    }
};
struct Release {
    std::shared_ptr<Gate> gate;
    ~Release() { gate->release.release(); }
};
class Alignment final : public CorrespondenceAlgorithm {
public:
    explicit Alignment(std::shared_ptr<Gate> gate) : gate(std::move(gate)) {}
    std::string_view identity() const noexcept override { return ExactLineCorrespondence{}.identity(); }
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        gate->hold();
        return ExactLineCorrespondence{}.compare(source, result);
    }
    std::shared_ptr<Gate> gate;
};
class Files final : public test_support::SyncFaults {
public:
    explicit Files(std::shared_ptr<Gate> gate) : gate(std::move(gate)) {}
    int sync(int descriptor) const override
    {
        gate->hold();
        return SyncFaults::sync(descriptor);
    }
    std::shared_ptr<Gate> gate;
};
FileSession captured(const std::shared_ptr<Gate> &gate, bool large = false)
{
    std::string text = "before\n";
    if (large)
        for (std::size_t i = 0; i < kLargeFileLines; ++i) text += "line\n";
    auto source = test_support::choiceFile(text, "other\n", text);
    return FileSession(
        "choices", "choices.txt", {text, {}, 0},
        {.snapshots = *source.snapshots(), .initializeChoices = true, .algorithm = std::make_shared<Alignment>(gate)});
}
void reached(const std::shared_ptr<Gate> &gate)
{
    bool ticked = false;
    QTimer::singleShot(0, [&] { ticked = true; });
    waitUntil([&] { return gate->entered && ticked; }, "worker computation blocked the event loop");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    test_support::Suite suite("SessionExecutionTests");
    for (const bool large : {false, true})
        suite.run(large ? "large admission computes on the worker" : "choice application computes on the worker", [=] {
            QTemporaryDir directory;
            auto gate = std::make_shared<Gate>();
            SessionStore store(directory.path() + "/recovery");
            CHECK(store.appendFile(captured(gate, large)));
            test_support::awaitRecovery(store);
            auto preview = previewWholeFile(*store.file("choices"), 1, SourceSide::Stage3);
            Release release{gate};
            gate->armed = true;
            auto command = large ? store.loadLargeFile("choices") : store.applyWholeFile("choices", preview, true);
            reached(gate);
            CHECK(!command.isFinished() && store.file("choices")->current().id == 1);
            CHECK(store.commandPending("choices"));
            CHECK(store.typeResult("choices", 1, {{0, 0}, "refused"}) == Transition::Blocked);
            gate->release.release();
            CHECK(await(command) == Transition::Applied);
            const auto journal = storage::RecoveryStore(store.recoveryDirectory()).read();
            CHECK(journal.succeeded && journal.transactions.size() == 2);
            CHECK(replayRecovery(storage::RecoveryStore(store.recoveryDirectory())).succeeded);
        });
    suite.run("deferred arguments and dependent commands keep their exact order", [] {
        QTemporaryDir directory;
        auto gate = std::make_shared<Gate>();
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(captured(gate)));
        test_support::awaitRecovery(store);
        Release release{gate};
        gate->armed = true;
        auto first = store.replaceDraft("choices", 1, {"owned temporary\n", {}, 0});
        reached(gate);
        auto second = store.undo("choices", 2);
        CHECK(!first.isFinished() && !second.isFinished());
        gate->release.release();
        CHECK(await(first) == Transition::Applied && await(second) == Transition::Applied);
        CHECK(store.file("choices")->current().id == 3);
        CHECK(store.file("choices")->current().draft.text == "before\n");
        CHECK(store.typeResult("choices", 3, {{0, 0}, "typed "}) == Transition::Applied);
        CHECK(await(store.recordCheckpoint("choices", 4, Outcome::Succeeded)) == Transition::Applied);
        auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(replay.succeeded && replay.files.front().publication().checkpoint() == 4);
        CHECK(replay.files.front().current().draft.text == "typed before\n");
    });
    for (const bool byteLimit : {false, true})
        for (const bool fail : {false, true})
            suite.run(std::string(byteLimit ? "byte limit: " : "count limit: ")
                          + (fail ? "failed writer retains refused typing through retry"
                                  : "stalled writer bounds typing and resumes admission"),
                      [=] {
                          QTemporaryDir directory;
                          auto gate = std::make_shared<Gate>();
                          auto files = std::make_shared<Files>(gate);
                          SessionStore store(directory.path() + "/recovery", files);
                          CHECK(store.appendFile(captured(std::make_shared<Gate>())));
                          test_support::awaitRecovery(store);
                          store.setExecutionLimits(
                              {byteLimit ? 2U : 1U, (byteLimit ? 1U : 2U) * RecoveryJournal::retainedBytes});
                          Release release{gate};
                          if (fail) files->arm(test_support::SyncFaults::Point::JournalSync);
                          gate->armed = true;
                          CHECK(store.typeResult("choices", 1, {{0, 0}, "a"}) == Transition::Applied);
                          CHECK(store.typeResult("choices", 2, {{1, 1}, "\nb"}) == Transition::Applied);
                          store.recordTyping();
                          reached(gate);
                          CHECK(store.hasPendingRecovery() && store.pendingDraft("choices")->text == "a\nbbefore\n");
                          CHECK(store.pendingError("choices").code == ProblemCode::RecoveryResourceLimit);
                          CHECK(store.typeResult("choices", 3, {{0, 0}, "overflow"}) == Transition::Blocked);
                          CHECK(await(store.undo("choices", 3)) == Transition::Blocked);
                          gate->release.release();
                          if (fail) {
                              waitUntil([&] { return store.file("choices")->current().id == 1; },
                                        "failed typing did not roll back");
                              const auto old =
                                  storage::RecoveryStore(store.recoveryDirectory()).read().transactions.back().id;
                              files->disarm();
                              CHECK(await(store.retryRecovery("choices")) == Transition::Applied);
                              const auto journal = storage::RecoveryStore(store.recoveryDirectory()).read();
                              CHECK(journal.transactions[1].id == old);
                          } else
                              test_support::awaitRecovery(store);
                          CHECK(!store.hasPendingRecovery() && !store.recording());
                          CHECK(store.file("choices")->current().draft.text == "a\nbbefore\n");
                          CHECK(replayRecovery(storage::RecoveryStore(store.recoveryDirectory()))
                                    .files.front()
                                    .current()
                                    .draft.text
                                == "a\nbbefore\n");
                          CHECK(await(store.undo("choices", 3)) == Transition::Applied);
                      });
    for (const bool throws : {false, true})
        suite.run(throws ? "unexpected worker failure has one terminal callback"
                         : "canceled future still delivers admitted work to its owner",
                  [=] {
                      QThreadPool pool;
                      QObject context;
                      auto gate = std::make_shared<Gate>();
                      Release release{gate};
                      gate->armed = true;
                      int count = 0, terminal = 0;
                      auto future = runOffThread(
                          pool, &context,
                          [gate, throws] {
                              gate->hold();
                              if (throws) throw std::runtime_error("outside consumer guards");
                              return 7;
                          },
                          [&](int result) {
                              ++count;
                              terminal = result;
                          },
                          [](std::exception_ptr) noexcept { return -1; });
                      reached(gate);
                      if (!throws) future.cancel();
                      gate->release.release();
                      waitUntil([&] { return count == 1; }, "terminal callback was lost");
                      CHECK(terminal == (throws ? -1 : 7) && future.isFinished());
                      QCoreApplication::processEvents();
                      CHECK(count == 1);
                  });
    suite.run("canceling command observation preserves its durable write and releases admission", [] {
        QTemporaryDir directory;
        auto gate = std::make_shared<Gate>();
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(captured(gate)));
        test_support::awaitRecovery(store);
        store.setExecutionLimits({1, RecoveryJournal::retainedBytes});
        Release release{gate};
        gate->armed = true;
        auto future = store.replaceDraft("choices", 1, {"admitted\n", {}, 0});
        reached(gate);
        future.cancel();
        gate->release.release();
        test_support::awaitRecovery(store);
        CHECK(future.isFinished() && future.isCanceled());
        CHECK(store.file("choices")->current().draft.text == "admitted\n");
        CHECK(await(store.undo("choices", 2)) == Transition::Applied);
        CHECK(replayRecovery(storage::RecoveryStore(store.recoveryDirectory())).files.front().current().draft.text
              == "before\n");
    });
    suite.run("store destruction drains command futures without owner event delivery", [] {
        QTemporaryDir directory;
        auto gate = std::make_shared<Gate>();
        auto store = std::make_unique<SessionStore>(directory.path() + "/recovery");
        CHECK(store->appendFile(captured(gate)));
        test_support::awaitRecovery(*store);
        Release release{gate};
        gate->armed = true;
        auto future = store->replaceDraft("choices", 1, {"drained\n", {}, 0});
        reached(gate);
        gate->release.release();
        store.reset();
        CHECK(future.isFinished() && future.result() == Transition::Applied);
        CHECK(replayRecovery(storage::RecoveryStore(directory.path() + "/recovery")).files.front().current().draft.text
              == "drained\n");
    });
    suite.run("asynchronous confirmation keeps the explicit preview until it commits", [] {
        QTemporaryDir directory;
        Application controller({.git = {},
                                .recoveryDirectory = directory.path() + "/recovery",
                                .mergetoolDirectory = directory.path() + "/socket"});
        auto &store = *controller.store();
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        CHECK(controller.selection()->select("choices"));
        auto &choices = *controller.choices();
        CHECK(choices.preview(ChoicePreviewFlow::Scope::WholeFile, "choices", "1", ChoicePreviewFlow::Side::RightSide));
        waitUntil([&] { return !choices.previewPending(); }, "whole-file preview did not settle");
        CHECK(choices.previewReady());
        CHECK(choices.apply(ChoicePreviewFlow::Scope::WholeFile, false));
        test_support::awaitRecovery(store);
        CHECK(!choices.wholeFilePreview().isEmpty() && store.file("choices")->current().id == 1);
        CHECK(choices.apply(ChoicePreviewFlow::Scope::WholeFile, true));
        test_support::awaitRecovery(store);
        CHECK(choices.wholeFilePreview().isEmpty() && store.file("choices")->current().id == 2);
    });
    suite.run("engine and storage share the normalized character policy", [] {
        for (const int control : {1, 13, 31, 127}) {
            const std::string text(1, static_cast<char>(control));
            CHECK(validUtf8(text) && !editableText(text));
            CHECK(!storage::serializeText(QByteArray::fromStdString(text), {}).document);
            FileSession file("id", "path", {"valid\n", {}, 0});
            CHECK(file.editResult(1, {{0, 0}, text}) == Transition::InvalidText);
        }
        CHECK(editableText("tab\tline\n\xc3\xb1"));
        const auto decoded = storage::decodeText("tab\r\nline\t");
        CHECK(decoded.document && decoded.document->utf8 == "tab\nline\t");
    });
    return suite.report();
}
