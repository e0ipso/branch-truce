// Editing the result in the window: typing, input methods, the shared edit actions and clipboard,
// previews and confirmations, and Save and discard.
#include "ComparisonCases.h"
#include "ComparisonEditor.h"
#include "ComparisonRecovery.h"
#include "ComparisonSessionChecks.h"
#include "ComparisonTextProbes.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"
#include "fixtures/GuiState.h"
#include "fixtures/SyncFaults.h"
#include <QClipboard>

using test_support::await;
using test_support::commandResult;

using namespace comparison_scene;

namespace {
void addNativeEditingCases(Group &scenes)
{
    scenes.run("native typing and Unicode selection replacement use command-owned undo redo and recovery", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("left\n", "right\n", "launch \xf0\x9f\x98\x80\n"));
        const auto initial = fixture.file().current();
        auto *editor = fixture.item("resultText");
        auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        CHECK(!document->isUndoRedoEnabled());
        CHECK(editor->setProperty("cursorPosition", 0));
        key(fixture, Qt::Key_A, Qt::NoModifier, "a");
        const auto typed = fixture.file().current();
        CHECK(typed.id == initial.id + 1 && typed.draft.text == "alaunch \xf0\x9f\x98\x80\n");
        checkRecovered(*controller.store(), *controller.files(), typed.draft);
        select(fixture, 8, 10);
        key(fixture, Qt::Key_E, Qt::NoModifier, QString::fromUtf8("\xc3\xa9"));
        const auto replaced = fixture.file().current();
        CHECK(replaced.id == typed.id + 1 && replaced.draft.text == "alaunch \xc3\xa9\n");
        CHECK(replaced.draft.decisions == initial.draft.decisions
              && fixture.text().toStdString() == replaced.draft.text);
        checkRecovered(*controller.store(), *controller.files(), replaced.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == typed.draft && fixture.text().toStdString() == typed.draft.text);
        checkRecovered(*controller.store(), *controller.files(), typed.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == initial.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft == typed.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft == replaced.draft && !document->isUndoAvailable());
        checkRecovered(*controller.store(), *controller.files(), replaced.draft);
    });
    scenes.run("shared edit actions follow selection and clipboard state in both controls", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("left\n", "right\n", "original\n"));
        auto *actions = fixture.object("resultEditActions");
        const auto action = [&](const char *name) {
            auto *result = actions->property(name).value<QObject *>();
            CHECK(result);
            return result;
        };
        const auto checkEnabled = [&](const char *name, const char *button, const char *entry, bool enabled) {
            auto *shared = action(name);
            CHECK(shared->property("enabled").toBool() == enabled);
            CHECK(toolbarEnabled(fixture, button) == enabled);
            CHECK(fixture.object(entry)->property("action").value<QObject *>() == shared);
            CHECK(fixture.object(entry)->property("enabled").toBool() == enabled);
        };
        const test_support::ScopedClipboard clipboard;
        QGuiApplication::clipboard()->setText("clipboard");
        CHECK(controller.editor()->editable());
        CHECK(!fixture.item("resultText")->property("readOnly").toBool());
        checkEnabled("copy", "copyResult", "resultMenuCopy", false);
        checkEnabled("paste", "pasteResult", "resultMenuPaste", true);
        key(fixture, Qt::Key_C, Qt::ControlModifier);
        CHECK(QGuiApplication::clipboard()->text() == "clipboard");
        select(fixture, 0, 4);
        checkEnabled("copy", "copyResult", "resultMenuCopy", true);
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "open"));
        settle([&] { return fixture.object("resultContextMenu")->property("opened").toBool(); });
        CHECK(fixture.item("resultMenuCopy")->isEnabled());
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "close"));
        settle([&] { return !fixture.object("resultContextMenu")->property("visible").toBool(); });
        key(fixture, Qt::Key_C, Qt::ControlModifier);
        CHECK(QGuiApplication::clipboard()->text() == "orig");
        select(fixture, 0, 0);
        checkEnabled("copy", "copyResult", "resultMenuCopy", false);
        QGuiApplication::clipboard()->clear();
        checkEnabled("paste", "pasteResult", "resultMenuPaste", false);
        CHECK(controller.store()->appendFile(FileSession(
            "outside", "diagram.png", {}, {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(controller.selection()->select("outside"));
        CHECK(!controller.editor()->editable());
        CHECK(fixture.item("resultText")->property("readOnly").toBool());
        CHECK(!action("paste")->property("enabled").toBool());
        CHECK(controller.selection()->select("choices"));
        QGuiApplication::clipboard()->setText("clipboard");
        checkEnabled("paste", "pasteResult", "resultMenuPaste", true);
    });
    scenes.run("the result menu opens from a right click, the Menu key and Shift+F10", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        settlePanes(fixture);
        auto *editor = fixture.item("resultText");
        auto *menu = fixture.object("resultContextMenu");
        const auto closeMenu = [&] {
            QTest::keyClick(fixture.window, Qt::Key_Escape);
            settle([&] { return !menu->property("visible").toBool(); });
        };
        editor->forceActiveFocus(Qt::TabFocusReason);
        settle([&] { return editor->hasActiveFocus(); });
        QTest::keyClick(fixture.window, Qt::Key_Menu);
        settle([&] { return menu->property("opened").toBool(); });
        closeMenu();
        editor->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(fixture.window, Qt::Key_F10, Qt::ShiftModifier);
        settle([&] { return menu->property("opened").toBool(); });
        closeMenu();
        // Another modifier with F10 is not a menu request.
        editor->forceActiveFocus(Qt::TabFocusReason);
        QSignalSpy shown(menu, SIGNAL(aboutToShow()));
        QTest::keyClick(fixture.window, Qt::Key_F10, Qt::ShiftModifier | Qt::ControlModifier);
        test_support::waitForFrames(fixture.window, "the Ctrl+Shift+F10 press is handled");
        CHECK(shown.isEmpty() && !menu->property("visible").toBool());
        const auto point = editor->mapToScene(QPointF(editor->width() / 2, 10)).toPoint();
        QTest::mouseClick(fixture.window, Qt::RightButton, Qt::NoModifier, point);
        settle([&] { return menu->property("opened").toBool(); });
        // Only the app's menu opens, never the editor's default text menu.
        int openMenus = 0;
        for (auto *popup : fixture.window->findChildren<QObject *>())
            if (popup->inherits("QQuickMenu") && popup->property("visible").toBool()) ++openMenus;
        CHECK(openMenus == 1);
        closeMenu();
    });
    scenes.run("lossless editing gates toolbar menu and native paste together", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("left\n", "right\n", "line\xe2\x80\xa8separator\n"));
        const auto before = fixture.file().current();
        const test_support::ScopedClipboard clipboard;
        QGuiApplication::clipboard()->setText("replacement");
        CHECK(!controller.editor()->editable());
        CHECK(fixture.item("resultText")->property("readOnly").toBool());
        CHECK(!toolbarEnabled(fixture, "pasteResult"));
        CHECK(!fixture.object("resultMenuPaste")->property("enabled").toBool());
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "open"));
        settle([&] { return fixture.object("resultContextMenu")->property("opened").toBool(); });
        CHECK(!fixture.item("resultMenuPaste")->isEnabled());
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "close"));
        settle([&] { return !fixture.object("resultContextMenu")->property("visible").toBool(); });
        CHECK(!controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Paste));
        key(fixture, Qt::Key_V, Qt::ControlModifier);
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
    });
    scenes.run("clipboard menus and shortcuts preserve atomic history and recovery", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("left\n", "right\n", "original\n"));
        const auto initial = fixture.file().current();
        const test_support::ScopedClipboard clipboard;
        CHECK(controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::SelectAll));
        CHECK(controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Copy));
        CHECK(QGuiApplication::clipboard()->text() == "original\n" && fixture.file().current().id == initial.id);
        QGuiApplication::clipboard()->setText(QString::fromUtf8("paste\t\xf0\x9f\x98\x80\n"));
        CHECK(controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Paste));
        fixture.settleCommand();
        const auto pasted = fixture.file().current();
        CHECK(pasted.id == initial.id + 1 && pasted.draft.text == "paste\t\xf0\x9f\x98\x80\n");
        checkRecovered(*controller.store(), *controller.files(), pasted.draft);
        select(fixture, 0, 5);
        key(fixture, Qt::Key_C, Qt::ControlModifier);
        CHECK(QGuiApplication::clipboard()->text() == "paste" && fixture.file().current().id == pasted.id);
        CHECK(controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Cut));
        fixture.settleCommand();
        const auto cut = fixture.file().current();
        CHECK(cut.id == pasted.id + 1 && cut.draft.text == "\t\xf0\x9f\x98\x80\n");
        checkRecovered(*controller.store(), *controller.files(), cut.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == pasted.draft);
        select(fixture, 0, 5);
        QGuiApplication::clipboard()->setText("keyboard paste");
        key(fixture, Qt::Key_V, Qt::ControlModifier);
        const auto keyboard = fixture.file().current();
        CHECK(keyboard.draft.text == "keyboard paste\t\xf0\x9f\x98\x80\n");
        checkRecovered(*controller.store(), *controller.files(), keyboard.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == pasted.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft == keyboard.draft);
        checkRecovered(*controller.store(), *controller.files(), keyboard.draft);
    });
    scenes.run("a file click refused while composing leaves the list on the selected row", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        auto captured = *test_support::batchFile().snapshots();
        captured.path = "other.txt";
        CHECK(
            controller.store()->appendFile(FileSession("other", captured.path, {captured.launchResult.text.utf8, {}, 0},
                                                       {.snapshots = captured, .initializeChoices = true})));
        auto *list = fixture.item("fileList");
        auto *other = test_support::visualButton(
            list, [](QQuickItem *item) { return item->property("fileId").toString() == QStringLiteral("other"); });
        CHECK(other);
        auto *editor = fixture.item("resultText");
        QInputMethodEvent preedit(QString::fromUtf8("\xe3\x81\x8b"), {});
        QCoreApplication::sendEvent(editor, &preedit);
        CHECK(editor->property("inputMethodComposing").toBool());
        CHECK(QMetaObject::invokeMethod(other, "clicked"));
        CHECK(controller.selection()->fileId() == QStringLiteral("choices"));
        CHECK(list->property("currentIndex").toInt() == controller.selection()->row());
        QInputMethodEvent commit;
        QCoreApplication::sendEvent(editor, &commit);
        CHECK(QMetaObject::invokeMethod(other, "clicked"));
        CHECK(controller.selection()->fileId() == QStringLiteral("other"));
        CHECK(list->property("currentIndex").toInt() == controller.selection()->row());
    });
    scenes.run("an input-method preedit stays pending and its commit is one recovered command", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile());
        const auto before = fixture.file().current();
        auto *editor = fixture.item("resultText");
        CHECK(editor->setProperty("cursorPosition", 0));
        QInputMethodEvent preedit(QString::fromUtf8("\xe3\x81\x8b"), {});
        QCoreApplication::sendEvent(editor, &preedit);
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
        QInputMethodEvent commit;
        commit.setCommitString(QString::fromUtf8("\xe3\x81\x8b"));
        QCoreApplication::sendEvent(editor, &commit);
        const auto chosen = fixture.file().current();
        CHECK(chosen.id == before.id + 1 && chosen.draft.text == std::string("\xe3\x81\x8b") + before.draft.text);
        checkRecovered(*controller.store(), *controller.files(), chosen.draft);
        key(fixture, Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == before.draft);
    });
}

void addChoicePreviewCases(Group &scenes)
{
    for (const auto &[scopeName, scope] :
         {std::pair{QStringLiteral("left"), ChoicePreviewFlow::NonConflicting::LeftChanges},
          std::pair{QStringLiteral("right"), ChoicePreviewFlow::NonConflicting::RightChanges},
          std::pair{QStringLiteral("both"), ChoicePreviewFlow::NonConflicting::BothChanges}}) {
        scenes.run("batch preview identifies its file and source scope without changing another file "
                       + scopeName.toStdString(),
                   [=] {
                       Application controller(test_support::windowConfig());
                       Fixture fixture(controller);
                       auto captured = *test_support::batchFile().snapshots();
                       captured.path = "other.txt";
                       CHECK(controller.store()->appendFile(
                           FileSession("other", captured.path, {captured.launchResult.text.utf8, {}, 0},
                                       {.snapshots = captured, .initializeChoices = true})));
                       const auto other = controller.store()->file("other")->current();
                       CHECK(test_support::previewResult(
                           controller, controller.choices()->previewNonConflicting("choices", "1", scope)));
                       const auto preview = controller.choices()->nonConflictingPreview();
                       CHECK(preview.value("path").toString() == "choices.txt");
                       CHECK(preview.value("scope").toString()
                             == (scopeName == "left"        ? "Left"
                                     : scopeName == "right" ? "Right"
                                                            : "Left and Right"));
                       CHECK(preview.value("changes").toList().size() == (scopeName == "both" ? 2 : 1));
                       for (const auto &change : preview.value("changes").toList()) {
                           const auto label = change.toMap().value("scope").toString();
                           CHECK(label.startsWith(scopeName == "right" ? "Right" : "Left")
                                 || (scopeName == "both" && label.startsWith("Right")));
                       }
                       CHECK(commandResult(controller, controller.choices()->applyNonConflicting()));
                       CHECK(controller.store()->file("other")->current().id == other.id);
                       CHECK(controller.store()->file("other")->current().draft == other.draft);
                       const auto replay =
                           replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
                       CHECK(replay.succeeded && replay.files.size() == 2);
                       for (const auto &file : replay.files)
                           CHECK(file.current().draft == controller.store()->file(file.id())->current().draft);
                   });
    }
    scenes.run("a replacement after a canceled preview respects worker admission and opens on retry", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        auto &store = *controller.store();
        CHECK(store.typeResult("choices", fixture.file().current().id, {{0, 0}, "typed "}) == Transition::Applied);
        const auto revision = fixture.file().current().id;
        const auto pending = fixture.file().pendingAlignment();
        CHECK(pending);
        CHECK(QMetaObject::invokeMethod(fixture.window, "chooseWholeFile",
                                        Q_ARG(int, static_cast<int>(ChoicePreviewFlow::Side::LeftSide))));
        CHECK(controller.choices()->previewPending());
        CHECK(!fixture.object("wholeFileDialog")->property("visible").toBool());
        // No event-loop turn delivers the first request's completion before its replacement.
        CHECK(
            store.installAlignment("choices", revision, std::make_shared<const CorrespondenceMap>(pending->realigned()))
            == Transition::Applied);
        CHECK(QMetaObject::invokeMethod(fixture.window, "chooseWholeFile",
                                        Q_ARG(int, static_cast<int>(ChoicePreviewFlow::Side::RightSide))));
        // The canceled request retains its worker slot until completion. A full
        // visual pool refuses the new request explicitly, without changing work.
        if (!controller.choices()->previewPending()) {
            CHECK(!controller.choices()->previewReady());
            CHECK(controller.feedback()->message()
                  == QStringLiteral("Computation exceeds the supported work limit. Nothing changed."));
            auto *workers = controller.revisionWorkers();
            CHECK(workers->admission.count == workers->admission.maxCount);
            const auto unchanged = fixture.file().current();
            test_support::waitUntil([&] { return workers->admission.count == 0; },
                                    "canceled visual work did not drain");
            CHECK(!fixture.object("wholeFileDialog")->property("visible").toBool());
            CHECK(!controller.choices()->previewReady() && fixture.file().current().id == unchanged.id
                  && fixture.file().current().draft == unchanged.draft);
            CHECK(QMetaObject::invokeMethod(fixture.window, "chooseWholeFile",
                                            Q_ARG(int, static_cast<int>(ChoicePreviewFlow::Side::RightSide))));
        }
        test_support::waitUntil([&] { return fixture.object("wholeFileDialog")->property("visible").toBool(); },
                                [&] {
                                    return std::string("replacement preview did not open: ")
                                        + controller.feedback()->message().toStdString() + "; pending="
                                        + std::to_string(controller.choices()->previewPending()) + "; ready="
                                        + std::to_string(controller.choices()->previewReady()) + "; requested="
                                        + std::to_string(fixture.window->property("previewRequested").toBool());
                                });
        CHECK(controller.choices()->wholeFilePreview().value("side").toString() == "Right");
        CHECK(QMetaObject::invokeMethod(fixture.object("wholeFileDialog"), "reject"));
    });
}

void addDraftPersistenceCases(Group &scenes)
{
    for (const int operation : {0, 1, 2}) {
        scenes.run(
            std::string("confirmation, cancellation and a stale application preserve the draft: ")
                + (operation == 0       ? "combination"
                       : operation == 1 ? "file batch"
                                        : "whole file"),
            [=] {
                Application controller(test_support::windowConfig());
                Fixture fixture(controller, operation == 1 ? test_support::batchFile() : test_support::choiceFile());
                const auto before = fixture.file().current();
                const auto target =
                    test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
                const auto open = [&] {
                    if (operation == 0)
                        CHECK(QMetaObject::invokeMethod(
                            fixture.window, "choose",
                            Q_ARG(QString, QString::fromStdString(target.source.contributionId)),
                            Q_ARG(int, static_cast<int>(ChoicePreviewFlow::Action::CombineRightFirst))));
                    else
                        CHECK(QMetaObject::invokeMethod(
                            fixture.window, operation == 1 ? "chooseNonConflicting" : "chooseWholeFile",
                            Q_ARG(int,
                                  operation == 1 ? static_cast<int>(ChoicePreviewFlow::NonConflicting::BothChanges)
                                                 : static_cast<int>(ChoicePreviewFlow::Side::RightSide))));
                    settle([&] { return !controller.choices()->previewPending(); });
                };
                const auto apply = [&] {
                    return operation == 0 ? commandResult(controller, controller.choices()->applyChoice(true))
                        : operation == 1  ? commandResult(controller, controller.choices()->applyNonConflicting())
                                          : commandResult(controller, controller.choices()->applyWholeFile());
                };
                const auto *dialogName = operation == 0 ? "replacementDialog"
                    : operation == 1                    ? "nonConflictingDialog"
                                                        : "wholeFileDialog";
                open();
                auto *dialog = fixture.object(dialogName);
                CHECK(dialog->property("visible").toBool());
                CHECK(QMetaObject::invokeMethod(dialog, "reject"));
                CHECK(controller.choices()->choicePreview().isEmpty()
                      && controller.choices()->nonConflictingPreview().isEmpty()
                      && controller.choices()->wholeFilePreview().isEmpty());
                CHECK(!apply() && fixture.file().current().id == before.id
                      && fixture.file().current().draft == before.draft);
                checkRecovered(*controller.store(), *controller.files(), before.draft);
                open();
                CHECK(commandResult(controller,
                                    controller.commands()->editResult("choices", QString::number(before.id), 0, 0,
                                                                      "newer manual edit\n")));
                const auto newer = fixture.file().current();
                // A refused confirmation keeps the dialog open and says why.
                CHECK(QMetaObject::invokeMethod(dialog, "confirm"));
                CHECK(dialog->property("visible").toBool() && dialog->property("refused").toBool());
                CHECK(dialog->property("refusalMessage").toString() == controller.feedback()->message()
                      && !controller.feedback()->message().isEmpty());
                CHECK(!apply() && fixture.file().current().id == newer.id
                      && fixture.file().current().draft == newer.draft);
                checkRecovered(*controller.store(), *controller.files(), newer.draft);
                CHECK(QMetaObject::invokeMethod(dialog, "reject"));
            });
    }
    scenes.run("Save and discard controls keep the app baseline and retry a failed Save", [] {
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
        CHECK(controller.feedback()->message() == "Draft saved in Branch Truce; repository file unchanged.");
        settle([&] { return fixture.item("commandMessage")->isVisible(); });
        CHECK(fixture.item("commandMessage")
                  ->property("text")
                  .toString()
                  .contains("Draft saved in Branch Truce; repository file unchanged"));
        CHECK(fixture.file().publication().checkpoint()
              && fixture.file().revision(*fixture.file().publication().checkpoint())->draft == saved);
        CHECK(!fixture.file().facts().unsaved && !fixture.file().publication().written()
              && !fixture.file().publication().staged());
        // A restart sees the Save and nothing written; SessionRecoveryTests checks the records themselves.
        {
            const auto replay = replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.front().publication().checkpoint()
                  && replay.files.front().revision(*replay.files.front().publication().checkpoint())->draft == saved);
            CHECK(!replay.files.front().publication().written() && !replay.files.front().publication().staged());
        }
        auto discarded = saved;
        discarded.text += "discarded edit\n";
        discarded.decisions["kept"] = {DecisionValue::Ignored, {}};
        CHECK(await(controller.store()->replaceDraft("choices", fixture.file().current().id, discarded))
              == Transition::Applied);
        settle([&] { return controller.selectedFile()->unsaved(); });
        CHECK(fixture.file().revision(*fixture.file().publication().checkpoint())->draft == saved);
        click(fixture, "discardUnsaved");
        CHECK(fixture.file().current().draft == saved && fixture.file().current().draft != discarded);
        CHECK(fixture.file().current().draft.decisions.at("kept").value == DecisionValue::Accepted);
        CHECK(fixture.text().toStdString() == saved.text);
        CHECK(!fixture.file().facts().unsaved && !fixture.file().publication().written()
              && !fixture.file().publication().staged());
        const auto replay = replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
        CHECK(replay.succeeded && replay.files.front().current().draft == saved);
        CHECK(replay.files.front().current().draft != discarded && !replay.files.front().publication().written()
              && !replay.files.front().publication().staged());

        CHECK(commandResult(controller,
                            controller.commands()->editResult(
                                "choices", QString::number(fixture.file().current().id),
                                static_cast<int>(QString::fromStdString(saved.text).size()),
                                static_cast<int>(QString::fromStdString(saved.text).size()), "retry me\n")));
        const auto edited = fixture.file().current().draft;
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            click(fixture, "saveDraft");
        }
        settle([&] { return controller.selectedFile()->recoveryState() == FileStates::RecoveryState::SaveFailed; });
        CHECK(fixture.file().current().draft == edited && fixture.file().facts().unsaved);
        CHECK(fixture.file().revision(*fixture.file().publication().checkpoint())->draft == saved);
        CHECK(fixture.text().toStdString() == edited.text);
        settle([&] { return fixture.item("commandMessage")->isVisible(); });
        CHECK(fixture.item("commandMessage")->property("text").toString().contains("Draft save failed"));
        CHECK(controller.feedback()->message() == "Draft save failed. The edited result is still available.");
        CHECK(fixture.item("retrySave")->isVisible());
        CHECK(!fixture.item("saveDraft")->isEnabled());
        CHECK(!toolbarEnabled(fixture, "discardUnsaved"));
        CHECK(!toolbarEnabled(fixture, "undoCommand"));
        CHECK(!fixture.file().publication().written() && !fixture.file().publication().staged());
        click(fixture, "retrySave");
        settle([&] { return !controller.selectedFile()->unsaved(); });
        CHECK(fixture.file().publication().checkpoint()
              && fixture.file().revision(*fixture.file().publication().checkpoint())->draft == edited);
        CHECK(!fixture.file().facts().unsaved && !fixture.item("retrySave")->isVisible());
        CHECK(controller.selectedFile()->status() == "Draft saved in Branch Truce; repository file unchanged");
        CHECK(!fixture.file().publication().written() && !fixture.file().publication().staged());
    });
}

}

void comparison_scene::addEditingCases(Groups groups)
{
    addNativeEditingCases(groups.scenes);
    addChoicePreviewCases(groups.scenes);
    addDraftPersistenceCases(groups.scenes);
}
