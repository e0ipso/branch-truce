// Worker gates separate request acceptance, visual publication, and retained admission.
#include "TestHarness.h"
#include "app/Application.h"
#include "app/WordDifferenceDecorations.h"
#include "core/ExactLineCorrespondence.h"
#include "core/internal/ComputationInput.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"
#include "fixtures/ChoiceFile.h"
#include "highlighting/PaneHighlighter.h"
#include "session/BackgroundAlignment.h"
#include <QGuiApplication>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <atomic>

using namespace branch_truce;
using test_support::await;
using test_support::waitUntil;
namespace {
struct Gate {
    std::atomic_bool armed{false};
    std::atomic_int entered{0};
    QSemaphore release;
    void hold()
    {
        if (!armed.exchange(false)) return;
        ++entered;
        if (!release.tryAcquire(1, 5000)) throw std::runtime_error("compute gate timed out");
    }
    ~Gate() { release.release(8); }
};
struct Release {
    std::shared_ptr<Gate> gate;
    ~Release() { gate->release.release(8); }
};
void progress(const Gate &gate, int entries = 1)
{
    bool tick = false;
    QTimer::singleShot(0, [&] { tick = true; });
    waitUntil([&] { return tick && gate.entered >= entries; }, "compute blocked the owner event loop");
}
Application::Config config(const QTemporaryDir &dir)
{
    return {.git = {}, .recoveryDirectory = dir.path() + "/recovery", .mergetoolDirectory = dir.path() + "/socket"};
}
void loaded(Application &app)
{
    CHECK(app.store()->appendFile(test_support::batchFile()));
    test_support::awaitRecovery(*app.store());
    CHECK(app.selection()->select("choices"));
    waitUntil([&] { return app.revisionWorkers()->admission.count == 0; }, "initial decoration did not settle");
}
class Alignment final : public CorrespondenceAlgorithm {
public:
    explicit Alignment(std::shared_ptr<Gate> gate) : gate(std::move(gate)) {}
    std::shared_ptr<Gate> gate;
    std::atomic_int *failure = nullptr;
    std::string_view identity() const noexcept override { return ExactLineCorrespondence{}.identity(); }
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        gate->hold();
        if (failure && *failure == 1) throw std::bad_alloc();
        if (failure && *failure == 2) throw std::logic_error("algorithm defect");
        return ExactLineCorrespondence{}.compare(source, result);
    }
};
FileSession provisional(const std::shared_ptr<Alignment> &algorithm)
{
    auto original = test_support::choiceFile("before\n", "other\n");
    FileSession file("choices", original.path(), original.current().draft,
                     {.snapshots = *original.snapshots(), .initializeChoices = true, .algorithm = algorithm});
    CHECK(file.editResult(1, {{0, 0}, "typed "}, {.realignment = Realignment::Deferred}) == Transition::Applied);
    return file;
}
}
namespace {
void addPreviewWorkerCases(test_support::Suite &suite)
{
    for (const int scope : {0, 1, 2})
        suite.run("preview work yields before publishing scope " + std::to_string(scope), [=] {
            QTemporaryDir dir;
            auto gate = std::make_shared<Gate>();
            Application application(config(dir));
            Release release{gate};
            loaded(application);
            application.revisionWorkers()->beforeCompute = [gate](bool words) {
                if (!words) gate->hold();
            };
            gate->armed = true;
            auto *choices = application.choices();
            QSignalSpy ready(choices, &ChoicePreviewFlow::ready);
            const auto target = test_support::choiceTarget(*application.store()->file("choices"), SourceSide::Stage2,
                                                           ChoiceScope::Section)
                                    .source.contributionId;
            CHECK(scope == 0 ? choices->previewChoice(QString("choices"), QString::number(1),
                                                      QString::fromStdString(target), ChoicePreviewFlow::Action::Accept)
                      : scope == 1 ? choices->previewNonConflicting(QString("choices"), QString::number(1),
                                                                    ChoicePreviewFlow::NonConflicting::BothChanges)
                                   : choices->previewWholeFile(QString("choices"), QString::number(1),
                                                               ChoicePreviewFlow::Side::RightSide));
            progress(*gate);
            CHECK(choices->previewPending() && !choices->previewReady() && ready.empty());
            CHECK(choices->choicePreview().isEmpty() && choices->wholeFilePreview().isEmpty());
            CHECK(!choices->applyWholeFile() && application.store()->file("choices")->current().id == 1);
            gate->release.release();
            waitUntil([&] { return ready.size() == 1; }, "preview did not publish");
            CHECK(choices->previewReady() && !choices->previewPending());
            CHECK(application.store()->file("choices")->current().id == 1 && !application.store()->recording());
        });
}
void addPreviewFenceCases(test_support::Suite &suite)
{
    for (const auto change : {"cancel", "replace", "revision", "selection", "session", "map", "blocked", "owner"})
        suite.run(std::string("blocked preview fences ") + change, [=] {
            QTemporaryDir dir;
            auto gate = std::make_shared<Gate>();
            Application application(config(dir));
            Release release{gate};
            loaded(application);
            application.revisionWorkers()->beforeCompute = [gate](bool words) {
                if (!words) gate->hold();
            };
            auto choices = std::make_unique<ChoicePreviewFlow>(*application.store(), *application.files(),
                                                               *application.editor(), *application.selection(),
                                                               *application.feedback(), *application.revisionWorkers());
            QSignalSpy ready(choices.get(), &ChoicePreviewFlow::ready);
            if (std::string(change) == "map") {
                CHECK(application.store()->typeResult("choices", 1, {{0, 0}, "typed "}) == Transition::Applied);
                test_support::awaitRecovery(*application.store());
            }
            const auto revision = application.store()->file("choices")->current().id;
            gate->armed = true;
            CHECK(choices->previewWholeFile("choices", QString::number(revision), ChoicePreviewFlow::Side::RightSide));
            progress(*gate);
            const std::string mode(change);
            if (mode == "cancel")
                choices->cancel();
            else if (mode == "replace") {
                CHECK(
                    choices->previewWholeFile("choices", QString::number(revision), ChoicePreviewFlow::Side::LeftSide));
                waitUntil([&] { return ready.size() == 1; }, "replacement did not publish ahead of obsolete work");
                CHECK(choices->wholeFilePreview().value("side").toString() == "Left");
            } else if (mode == "revision" || mode == "map") {
                CHECK(application.store()->typeResult("choices", revision, {{0, 0}, "newer "}) == Transition::Applied);
            } else if (mode == "selection") {
                CHECK(application.store()->appendFile(FileSession("other", "other.txt", {})));
                CHECK(application.selection()->select("other"));
            } else if (mode == "blocked")
                application.store()->recordRecoveryFailure("choices", ProblemCode::RecoveryStoreUnavailable);
            else if (mode == "session")
                application.store()->replaceFiles({test_support::batchFile()});
            else
                choices.reset();
            gate->release.release();
            waitUntil([&] { return application.revisionWorkers()->admission.count == 0; }, "stale reservation leaked");
            if (choices) {
                CHECK(!choices->previewPending());
                CHECK(ready.size() == (mode == "replace" ? 1 : 0));
                CHECK(mode == "replace" || choices->wholeFilePreview().isEmpty());
            }
        });
}
void addWordWorkerCases(test_support::Suite &suite)
{
    for (const int fault : {1, 2})
        suite.run("preview exceptions are typed " + std::to_string(fault), [=] {
            QTemporaryDir dir;
            Application application(config(dir));
            loaded(application);
            application.revisionWorkers()->beforeCompute = [fault](bool words) {
                if (words) return;
                if (fault == 1) throw std::bad_alloc();
                throw std::logic_error("algorithm defect");
            };
            auto *choices = application.choices();
            QSignalSpy ready(choices, &ChoicePreviewFlow::ready);
            CHECK(choices->previewWholeFile("choices", "1", ChoicePreviewFlow::Side::RightSide));
            waitUntil([&] { return !choices->previewPending(); }, "failure never terminated");
            CHECK(!choices->previewReady() && ready.empty() && choices->wholeFilePreview().isEmpty());
            CHECK(application.revisionWorkers()->admission.count == 0);
            CHECK(application.feedback()->message().contains(fault == 1 ? "memory" : "compute"));
        });
    suite.run("word ranges coalesce immutable inputs and preserve independent document formatting", [] {
        QTemporaryDir dir;
        auto gate = std::make_shared<Gate>();
        QThreadPool pool;
        RevisionWorkers workers(pool);
        SessionStore store(dir.path() + "/recovery");
        Release release{gate};
        CHECK(store.appendFile(test_support::choiceFile("α 😀 left\n", "α 😀 right\n")));
        test_support::awaitRecovery(store);
        QTextDocument document(QString::fromUtf8("α 😀 left\n"));
        QTextCursor cursor(&document);
        cursor.setPosition(0);
        cursor.setPosition(1, QTextCursor::KeepAnchor);
        QTextCharFormat independent;
        independent.setFontItalic(true);
        cursor.setCharFormat(independent);
        PaneHighlighter highlighter(&document);
        highlighter.attach();
        QObject editor;
        editor.setProperty("wordDifferenceStyle", QVariantMap{{"pane", "stage2"}, {"active", QColor(Qt::blue)}});
        WordDifferenceDecorations decorations(store, workers);
        workers.beforeCompute = [gate](bool words) {
            if (words) gate->hold();
        };
        gate->armed = true;
        decorations.setFile("choices");
        decorations.attach(&editor, &document);
        progress(*gate);
        CHECK(highlighter.decorations().isEmpty());
        auto source = test_support::choiceFile("prefix α 😀 blue\n", "prefix α 😀 red\n");
        CHECK(store.appendFile(FileSession("other", source.path(), source.current().draft,
                                           {.snapshots = *source.snapshots(), .initializeChoices = true})));
        test_support::awaitRecovery(store);
        decorations.setFile("other");
        document.setPlainText(QString::fromUtf8("prefix α 😀 blue\n"));
        CHECK(highlighter.decorations().isEmpty());
        for (RevisionId revision = 1; revision < 3; ++revision)
            CHECK(store.typeResult("other", revision, {{0, 0}, "typed "}) == Transition::Applied);
        CHECK(workers.admission.count == 1);
        test_support::awaitRecovery(store);
        const auto aligned = await(store.awaitAlignment("other", 3));
        CHECK(aligned == Transition::Applied || aligned == Transition::NoChange);
        gate->release.release();
        waitUntil([&] { return !highlighter.decorations().isEmpty(); }, "latest word ranges did not publish");
        CHECK(highlighter.decorations().size() == 1);
        CHECK(highlighter.decorations().front().start == 12 && highlighter.decorations().front().length == 4);
        CHECK(store.file("other")->current().id == 3 && !store.recording());
        const auto before = document.toPlainText();
        cursor.setPosition(0);
        cursor.setPosition(1, QTextCursor::KeepAnchor);
        cursor.setCharFormat(independent);
        decorations.refresh();
        CHECK(cursor.charFormat().fontItalic() && document.toPlainText() == before);
    });
    for (const int terminal : {0, 1, 2})
        suite.run("word computation releases its terminal reservation " + std::to_string(terminal), [=] {
            QTemporaryDir dir;
            auto gate = std::make_shared<Gate>();
            QThreadPool pool;
            RevisionWorkers workers(pool);
            SessionStore store(dir.path() + "/recovery");
            Release release{gate};
            CHECK(store.appendFile(test_support::choiceFile("one left\n", "one right\n")));
            test_support::awaitRecovery(store);
            QTextDocument document("one left\n");
            PaneHighlighter highlighter(&document);
            highlighter.attach();
            QObject editor;
            editor.setProperty("wordDifferenceStyle", QVariantMap{{"pane", "stage2"}, {"active", QColor(Qt::blue)}});
            auto decorations = std::make_unique<WordDifferenceDecorations>(store, workers);
            workers.beforeCompute = [gate, terminal](bool words) {
                if (!words) return;
                gate->hold();
                if (terminal == 1) throw std::bad_alloc();
                if (terminal == 2) throw std::logic_error("word algorithm defect");
            };
            gate->armed = true;
            decorations->setFile("choices");
            decorations->attach(&editor, &document);
            progress(*gate);
            if (terminal == 0) decorations.reset();
            gate->release.release();
            waitUntil([&] { return workers.admission.count == 0; }, "word terminal did not release accounting");
            QCoreApplication::processEvents();
            CHECK(highlighter.decorations().isEmpty() && workers.admission.bytes == 0);
            // A failure is terminal for this input, not an automatic retry loop.
            CHECK(gate->entered == 1 && store.file("choices")->current().id == 1 && !store.recording());
        });
}
void addAlignmentWorkerCases(test_support::Suite &suite)
{
    suite.run("alignment refusal and destruction complete observers explicitly", [] {
        QTemporaryDir dir;
        auto gate = std::make_shared<Gate>();
        auto algorithm = std::make_shared<Alignment>(gate);
        SessionStore store(dir.path() + "/recovery");
        QObject::disconnect(&store, nullptr, nullptr, nullptr);
        auto file = provisional(algorithm);
        store.replaceFiles({file});
        test_support::awaitRecovery(store);
        {
            BackgroundAlignment refused(store, nullptr, {2, 1});
            CHECK(await(refused.waitFor("choices", 2)) == Transition::ResourceLimit);
            CHECK(refused.admission().count == 0 && refused.admission().bytes == 0);
        }
        // A zero-count queue retains no map. Teardown still terminates its waiter.
        QFuture<Transition> waiting;
        {
            BackgroundAlignment queued(store, nullptr, {0, 268435456});
            waiting = queued.waitFor("choices", 2);
            CHECK(!waiting.isFinished() && queued.admission().count == 0);
        }
        CHECK(await(waiting) == Transition::Blocked);
    });
    for (const bool byteLimit : {false, true})
        suite.run(
            byteLimit ? "alignment retains bytes across replacements" : "alignment retains count across replacements",
            [=] {
                QTemporaryDir dir;
                auto gate = std::make_shared<Gate>();
                auto algorithm = std::make_shared<Alignment>(gate);
                // Isolate this owned alignment instance from the store's default observer.
                SessionStore store(dir.path() + "/recovery");
                QObject::disconnect(&store, nullptr, nullptr, nullptr);
                auto first = provisional(algorithm);
                const auto bytes = 2 * first.correspondence()->retainedBytes() + 4 * kMaxMetadataBytes
                    + detail::captureBytes(*first.snapshots());
                BackgroundAlignment alignment(store, nullptr, {2, byteLimit ? bytes : 2 * bytes});
                Release release{gate};
                const int admitted = byteLimit ? 1 : 2;
                std::vector<QFuture<Transition>> stale;
                for (int generation = 0; generation < 12; ++generation) {
                    auto next = generation == 0 ? first : provisional(algorithm);
                    gate->armed = generation < admitted;
                    store.replaceFiles({std::move(next)});
                    test_support::awaitRecovery(store);
                    stale.push_back(alignment.waitFor("choices", 2));
                    if (generation < admitted) progress(*gate, generation + 1);
                    CHECK(alignment.admission().count == static_cast<std::size_t>(std::min(generation + 1, admitted)));
                    CHECK(alignment.admission().bytes == alignment.admission().count * bytes);
                    CHECK(alignment.admission().bytes <= alignment.admission().maxBytes);
                }
                auto latest = alignment.waitFor("choices", 2);
                for (std::size_t i = 0; i + 1 < stale.size(); ++i) CHECK(await(stale[i]) == Transition::StaleRevision);
                gate->release.release(admitted);
                CHECK(await(latest) == Transition::Applied);
                CHECK(await(stale.back()) == Transition::Applied);
                waitUntil([&] { return alignment.admission().count == 0; }, "alignment did not reclaim stale work");
                CHECK(alignment.admission().bytes == 0 && !store.file("choices")->pendingAlignment());
            });
    for (const int fault : {1, 2})
        suite.run("alignment failure terminates without retry " + std::to_string(fault), [=] {
            QTemporaryDir dir;
            auto gate = std::make_shared<Gate>();
            auto algorithm = std::make_shared<Alignment>(gate);
            std::atomic_int failure{0};
            algorithm->failure = &failure;
            SessionStore store(dir.path() + "/recovery");
            QObject::disconnect(&store, nullptr, nullptr, nullptr);
            BackgroundAlignment alignment(store);
            auto file = provisional(algorithm);
            failure = fault;
            store.replaceFiles({std::move(file)});
            const auto expected = fault == 1 ? Transition::OutOfMemory : Transition::ComputationFailed;
            CHECK(await(alignment.waitFor("choices", 2)) == expected);
            CHECK(await(alignment.waitFor("choices", 2)) == expected);
            CHECK(alignment.admission().count == 0 && store.file("choices")->pendingAlignment());
            test_support::awaitRecovery(store);
        });
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    test_support::Suite suite("RevisionComputationTests");
    addPreviewWorkerCases(suite);
    addPreviewFenceCases(suite);
    addWordWorkerCases(suite);
    addAlignmentWorkerCases(suite);
    return suite.report();
}
