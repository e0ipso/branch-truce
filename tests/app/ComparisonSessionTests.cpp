// The session around the file: a failed discard, Mark resolved, the close handoff, large files and
// how notices show paths.
#include "ComparisonCases.h"
#include "ComparisonEditor.h"
#include "ComparisonRecovery.h"
#include "ComparisonSessionChecks.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"
#include "fixtures/FileToolbar.h"
#include "fixtures/GuiState.h"
#include "fixtures/SyncFaults.h"
#include <QClipboard>

using test_support::await;

using namespace comparison_scene;

void comparison_scene::addSessionCases(Groups groups)
{
    auto &scenes = groups.scenes;
    scenes.run("a failed Discard shows the restored result and retry finishes it", [] {
        const auto faults = std::make_shared<test_support::SyncFaults>();
        auto config = test_support::windowConfig();
        config.files = faults;
        Application controller(std::move(config));
        Fixture fixture(controller);
        auto saved = fixture.file().current().draft;
        saved.text += "saved app draft\n";
        saved.decisions["kept"] = {DecisionValue::Accepted, 4};
        CHECK(await(controller.store()->replaceDraft("choices", fixture.file().current().id, saved))
              == Transition::Applied);
        settle([&] { return controller.selectedFile()->unsaved(); });
        click(fixture, "saveDraft");
        auto discarded = saved;
        discarded.text += "discarded edit\n";
        discarded.decisions["kept"] = {DecisionValue::Ignored, {}};
        CHECK(await(controller.store()->replaceDraft("choices", fixture.file().current().id, discarded))
              == Transition::Applied);
        settle([&] { return controller.selectedFile()->unsaved(); });
        auto *discardButton = test_support::fileToolbarControl(fixture.window, "discardUnsaved");
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            clickItem(fixture, discardButton);
            CHECK_EQ(faults->failures.load(), 1);
        }
        settle([&] { return controller.files()->hasPendingRecovery(); });
        const auto message = controller.feedback()->message();
        CHECK(message == "Discard was not completed. The restored result is shown until retry finishes it.");
        CHECK(!message.contains(QStringLiteral("edited result is unchanged")));
        CHECK(fixture.text().toStdString() == saved.text);
        CHECK(controller.selectedFile()->pendingText().toStdString() == saved.text);
        CHECK(fixture.item("pendingInputText")->property("text").toString().toStdString() == saved.text);
        CHECK(fixture.file().current().draft == discarded);
        CHECK(controller.selectedFile()->resultText().toStdString() == discarded.text);
        CHECK(fixture.item("retryRecovery")->isVisible() && fixture.item("retryRecovery")->isEnabled());
        CHECK(!toolbarEnabled(fixture, "discardUnsaved"));
        CHECK(!fixture.item("markResolved")->isEnabled());
        storage::RecoveryStore store(controller.store()->recoveryDirectory());
        const auto pending = store.read();
        CHECK(pending.succeeded);
        const auto records = pending.transactions.size();
        const auto request = pending.transactions.back().id;
        click(fixture, "retryRecovery");
        settle([&] { return !controller.files()->hasPendingRecovery(); });
        CHECK(fixture.file().current().draft == saved && fixture.file().current().draft != discarded);
        CHECK(fixture.text().toStdString() == saved.text);
        CHECK(controller.feedback()->message()
              == "Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.");
        CHECK(!fixture.file().facts().unsaved && !fixture.file().publication().written()
              && !fixture.file().publication().staged());
        const auto committed = store.read();
        CHECK(committed.succeeded && committed.transactions.size() == records);
        // The retry committed the discard's one transaction, not another.
        CHECK(committed.transactions.back().id == request);
        const auto replay = replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
        CHECK(replay.succeeded && replay.files.front().current().draft == saved);
        CHECK(!replay.files.front().publication().written() && !replay.files.front().publication().staged());
    });
    scenes.run("Mark resolved confirms, rejects a stale revision and keeps a failed write visible", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        const auto text = fixture.text();
        CHECK(fixture.file().facts().undecided > 0 && fixture.file().facts().unsaved
              && !fixture.file().publication().reviewed());
        CHECK(fixture.item("markResolved")->isVisible() && fixture.item("markResolved")->isEnabled());
        CHECK(!fixture.item("resolutionNotice")->isVisible());
        click(fixture, "markResolved");
        CHECK(controller.commands()->resolutionConfirmation());
        CHECK(controller.feedback()->message().contains(QStringLiteral("intentional")));
        CHECK(fixture.text() == text && !fixture.file().publication().reviewed() && fixture.file().facts().unsaved);
        test_support::waitForLayout(fixture.window, QStringLiteral("confirmResolution"), 60);
        test_support::waitForLayout(fixture.window, QStringLiteral("cancelResolution"), 60);
        click(fixture, "cancelResolution");
        CHECK(!controller.commands()->resolutionConfirmation());
        CHECK(controller.feedback()->message().contains(QStringLiteral("unchanged")));
        CHECK(fixture.text() == text && fixture.file().current().draft.text == text.toStdString());
        CHECK(!controller.commands()->markResolved(QStringLiteral("choices"), QStringLiteral("999"), true));
        CHECK(controller.feedback()->message().contains(QStringLiteral("changed")));
        CHECK(fixture.text() == text && !fixture.file().publication().reviewed() && !fixture.file().facts().resolved);
        click(fixture, "markResolved");
        click(fixture, "confirmResolution");
        settle([&] { return controller.selectedFile()->status() == QStringLiteral("Repository write failed"); });
        CHECK(fixture.text() == text);
        CHECK(fixture.file().publication().reviewed() && fixture.file().publication().checkpoint()
              && !fixture.file().publication().written());
        CHECK(!fixture.file().facts().resolved && !fixture.file().publication().staged());
        settle([&] { return fixture.item("fileFailureNotice")->isVisible(); });
        CHECK(fixture.item("fileFailureNotice")
                  ->property("text")
                  .toString()
                  .contains(QStringLiteral("Repository write failed")));
        CHECK(!fixture.item("parentStagingState")->isVisible());
    });
    scenes.run("closing with work outstanding opens the handoff and each route returns to its file", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        auto *files = controller.store();
        CHECK(files->appendFile(namedChoiceFile("saved")));
        CHECK(await(files->recordCheckpoint("saved", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(files->appendFile(namedChoiceFile("published")));
        CHECK(await(files->beginMarkResolved("published", 1, true)) == Transition::Applied);
        CHECK(await(files->recordCheckpoint("published", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(files->recordWrite("published", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(files->recordStage("published", 1, Outcome::Succeeded, "verified-object", "100644"))
              == Transition::Applied);
        CHECK(files->file("published")->facts().resolved);
        CHECK(files->appendFile(FileSession("outside", "diagram.png", {},
                                            {.support = Support::OutsideApp, .supportDetail = "image content"})));

        auto *dialog = fixture.object("handoffDialog");
        const auto opened = [&] { return dialog->property("opened").toBool(); };
        CHECK(!dialog->property("visible").toBool());
        // The window close handler keeps the window and opens the handoff.
        fixture.window->close();
        settleFor("handoff opened by close", opened);
        CHECK(fixture.window->isVisible() && controller.completion()->outcome().isEmpty());
        CHECK(fixture.item("handoffOutcome")->property("text").toString()
              == QStringLiteral("3 of 4 files in this session remain outstanding. This session is not finished."));
        CHECK(!fixture.item("handoffFinish")->isVisible());

        QStringList outstanding;
        QMap<QString, QString> categories;
        for (const auto &group : controller.completion()->handoffData().value(QStringLiteral("groups")).toList())
            for (const auto &entry : group.toMap().value(QStringLiteral("entries")).toList()) {
                const auto fields = entry.toMap();
                categories.insert(fields.value(QStringLiteral("fileId")).toString(),
                                  fields.value(QStringLiteral("category")).toString());
                if (fields.value(QStringLiteral("outstanding")).toBool())
                    outstanding << fields.value(QStringLiteral("fileId")).toString();
            }
        CHECK(outstanding.size() == 3 && outstanding.contains(QStringLiteral("choices")));
        CHECK(categories.value(QStringLiteral("saved")) == QStringLiteral("saved-draft"));
        CHECK(categories.value(QStringLiteral("outside")) == QStringLiteral("outside-app"));
        CHECK(categories.value(QStringLiteral("published")) == QStringLiteral("published"));
        for (const auto &id : outstanding) {
            CHECK(controller.selection()->select(QStringLiteral("published")));
            if (!opened()) click(fixture, "openHandoff");
            settleFor("handoff reopened for " + id.toStdString(), opened);
            const auto route = QStringLiteral("handoffRoute-") + id;
            clickItem(fixture, waitForLayout(fixture, route));
            // Until its fade-out ends the closing dialog's overlay takes the next click.
            settleFor("handoff closed by route " + id.toStdString(),
                      [&] { return !dialog->property("visible").toBool(); });
            if (controller.selection()->fileId() != id)
                throw std::runtime_error(
                    ("route " + id + " selected " + controller.selection()->fileId()).toStdString());
            CHECK(controller.selection()->fileId() == id);
        }
        CHECK(controller.selection()->select(QStringLiteral("outside")));
        CHECK(fixture.item("selectedProblem")->property("explanation").toString().contains("outstanding"));
        expectDiagnostic(fixture, QStringLiteral("image content"));

        int ended = 0;
        bool clean = true;
        QObject::connect(controller.completion(), &SessionCompletion::sessionEnded, &controller,
                         [&](const QString &, bool value) {
                             ++ended;
                             clean = value;
                         });
        click(fixture, "openHandoff");
        settleFor("handoff opened for leave", opened);
        auto *leave = waitForLayout(fixture, QStringLiteral("handoffLeave"));
        CHECK(leave == fixture.item("handoffLeave") && leave->isEnabled());
        clickItem(fixture, leave);
        CHECK(ended == 1 && !clean);
        CHECK(controller.completion()->outcome().startsWith(QStringLiteral("Left with 3 of 4 files outstanding.")));
        CHECK(!controller.completion()->leave() && ended == 1);
        CHECK(QMetaObject::invokeMethod(dialog, "close"));
        settleFor("handoff closed after leave", [&] { return !opened(); });
        // After the deliberate leave the close handler accepts the close.
        const test_support::ScopedQuitOnClose keepRunning(false);
        fixture.window->close();
        settleFor("window closed after leave", [&] { return !fixture.window->isVisible(); });
        CHECK(!opened() && ended == 1);
    });
    scenes.run("an insertion past 4000 lines is acknowledged", [] {
        const auto atThreshold = [] {
            std::string text;
            text.reserve(4000 * 2);
            for (int i = 0; i < 4000; ++i) text += "x\n";
            return text;
        }();
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("x\n", "x\n", atThreshold));
        auto *editor = fixture.item("resultText");
        const auto before = fixture.file().current();
        CHECK(!editor->property("readOnly").toBool() && editor->isEnabled());
        CHECK(fixture.text().toStdString() == atThreshold);
        select(fixture, 0, 0);
        const test_support::ScopedClipboard clipboard;
        QGuiApplication::clipboard()->setText(QStringLiteral("\n"));
        CHECK(controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Paste));
        const auto edited = fixture.file().current();
        CHECK(edited.id == before.id + 1 && edited.draft.text == "\n" + atThreshold);
        CHECK(fixture.text().toStdString() == edited.draft.text);
        CHECK(!editor->property("readOnly").toBool() && editor->isEnabled());
        CHECK(controller.feedback()->message().isEmpty());
        checkRecovered(*controller.store(), *controller.files(), edited.draft);
    });
    scenes.run("a file over 4000 lines loads only after the user confirms it", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        const auto large = [] {
            std::string text;
            text.reserve(4001 * 2);
            for (int i = 0; i < 4001; ++i) text += "x\n";
            return text;
        }();
        auto sources = *test_support::choiceFile("x\n", "x\n", large).snapshots();
        sources.path = "large.txt";
        CHECK(controller.store()->appendFile(FileSession(
            "large", "large.txt", {large, {}, 0}, {.snapshots = std::move(sources), .initializeChoices = true})));
        const auto *held = controller.store()->file("large");
        CHECK(held && held->support() == Support::LargeFile && !held->correspondence());
        CHECK(controller.selection()->select(QStringLiteral("large")));
        auto *message = fixture.item("selectedProblem");
        settleFor("large file placeholder",
                  [&] { return message->isVisible() && !fixture.item("resultText")->isVisible(); });
        CHECK(message->property("text").toString() == QStringLiteral("The file is over 4000 lines"));
        CHECK(message->property("explanation").toString() == QStringLiteral("Are you sure you want to load it?"));
        auto *load = fixture.object("loadLargeFileAction");
        CHECK(message->property("helpfulAction").value<QObject *>() == load);
        // The placeholder lays out again when it switches files, so click once it holds still.
        auto *button = test_support::waitForStable(
            fixture.window, "Load button",
            [&] {
                return test_support::visualButton(
                    message, [&](QQuickItem *item) { return item->property("action").value<QObject *>() == load; });
            },
            60);
        CHECK(button->isEnabled());
        CHECK(fixture.text().isEmpty() && controller.selectedFile()->property("resultText").toString().isEmpty());
        CHECK(button->property("text").toString() == QStringLiteral("Load"));
        clickItem(fixture, button);
        settleFor("large file loaded",
                  [&] { return fixture.item("resultText")->isVisible() && fixture.text().toStdString() == large; });
        const auto *loaded = controller.store()->file("large");
        CHECK(loaded->support() == Support::Supported && loaded->correspondence());
        CHECK(loaded->current().id == 2 && !loaded->canUndo());
        CHECK(!fixture.item("resultText")->property("readOnly").toBool());
        // The journal replays the load, but a load alone is not work to recover.
        const auto replay = replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
        CHECK(replay.succeeded && !replay.userWork && !replay.editedFiles.contains("large"));
        const auto recovered = std::find_if(replay.files.begin(), replay.files.end(),
                                            [](const FileSession &candidate) { return candidate.id() == "large"; });
        CHECK(recovered != replay.files.end() && recovered->support() == Support::Supported);
        CHECK(recovered->current().id == 2 && recovered->current().draft == loaded->current().draft);
    });
    scenes.run("notices and tooltips show a path as typed and load nothing it names", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        // A file name cannot hold a slash, so the image address spells its slashes as entities.
        const auto path =
            QStringLiteral("<b>x</b><img src=\"http:&#47;&#47;127.0.0.1:9&#47;probe\">.txt").toStdString();
        auto sources = *test_support::choiceFile().snapshots();
        sources.path = path;
        const auto launch = sources.launchResult.text.utf8;
        CHECK(controller.store()->appendFile(FileSession(
            "crafted", path, {launch, {}, 0}, {.snapshots = std::move(sources), .initializeChoices = true})));
        const auto shown = controller.files()->displayPath("crafted");
        CHECK(shown.toStdString() == path);
        CHECK(controller.selection()->select("crafted"));
        auto *pathLabel = fixture.item("selectedPath");
        settle([&] { return pathLabel->property("text").toString() == shown; });
        QString renderedPath;
        const std::function<void(QQuickItem *)> readPath = [&](QQuickItem *item) {
            if (!item->isVisible()) return;
            if (item->inherits("QQuickText")) {
                CHECK(item->property("textFormat").toInt() == Qt::PlainText);
                renderedPath += item->property("text").toString();
            }
            for (auto *child : item->childItems()) readPath(child);
        };
        readPath(pathLabel);
        CHECK(renderedPath == shown);
        const auto message = QStringLiteral("Waiting for Git to stage %1.").arg(shown);
        // Every label inside the item must show the message as plain characters. A label
        // that took it as rich text reports the markup it built instead.
        QStringList problems;
        const auto expectPlain = [&](QQuickItem *root, const QString &what) {
            int showing = 0;
            const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
                for (auto *child : item->childItems()) visit(child);
                if (!item->inherits("QQuickText") && !item->inherits("QQuickTextEdit")) return;
                // Rich text reads back as the HTML it built, which still holds these words.
                const auto text = item->property("text").toString();
                if (!text.contains(QStringLiteral("Waiting for Git to stage"))) return;
                ++showing;
                const auto *document = item->property("textDocument").value<QQuickTextDocument *>();
                const auto rendered = document ? document->textDocument()->toPlainText() : text;
                if (item->property("textFormat").toInt() != Qt::PlainText || text != message || rendered != message)
                    problems.append(what + QStringLiteral(" shows ") + rendered);
            };
            visit(root);
            if (showing == 0) problems.append(what + QStringLiteral(" has no label"));
        };
        for (const auto *name : {"handoffOutcome", "inventoryWarning", "recoveryProblem", "commandMessage",
                                 "parentStagingState", "mergetoolHolding", "mergetoolWait", "choiceNotice"}) {
            auto *notice = fixture.item(name);
            CHECK(notice->setProperty("visible", true) && notice->setProperty("text", message));
        }
        for (const auto *name : {"sessionPlaceholder", "selectedProblem"}) {
            auto *placeholder = fixture.item(name);
            CHECK(placeholder->setProperty("visible", true) && placeholder->setProperty("text", message)
                  && placeholder->setProperty("explanation", message));
        }
        // Attached tooltips share one popup per engine.
        auto *heading = fixture.item("repositoryHeading");
        QQmlExpression shared(qmlContext(heading), heading, QStringLiteral("ToolTip.toolTip"));
        auto *tooltip = shared.evaluate().value<QObject *>();
        CHECK(tooltip && !shared.hasError());
        CHECK(tooltip->setProperty("parent", QVariant::fromValue(heading)) && tooltip->setProperty("text", message)
              && tooltip->setProperty("visible", true));
        // Every label and the tooltip are laid out, and any image they named requested, by then.
        test_support::waitForFrames(fixture.window, "the notices and the tooltip are drawn");
        const auto requested = !fixture.networkRequests().isEmpty();
        for (const auto *name :
             {"handoffOutcome", "inventoryWarning", "recoveryProblem", "commandMessage", "parentStagingState",
              "mergetoolHolding", "mergetoolWait", "choiceNotice", "sessionPlaceholder", "selectedProblem"})
            expectPlain(fixture.item(name), QString::fromLatin1(name));
        auto *tooltipContent = tooltip->property("contentItem").value<QQuickItem *>();
        CHECK(tooltipContent);
        expectPlain(tooltipContent, QStringLiteral("tooltip"));
        if (requested) problems.append(QStringLiteral("a notice requested the image it names"));
        CHECK(tooltip->setProperty("visible", false));
        if (!problems.isEmpty()) throw std::runtime_error(problems.join(QStringLiteral("; ")).toStdString());
    });
}
