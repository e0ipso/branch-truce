// Editing the result in the window: typing, input methods, the shared edit actions and clipboard,
// previews and confirmations, and Save and discard.
#include "ComparisonScene.h"

using namespace comparison_scene;

void comparison_scene::addEditingCases(Groups groups)
{
    auto &scenes = groups.scenes;
    scenes.run("native typing and Unicode selection replacement use command-owned undo redo and recovery", [] {
        Fixture fixture(test_support::choiceFile("left\n", "right\n", "launch \xf0\x9f\x98\x80\n"));
        const auto initial = fixture.file().current();
        auto *editor = fixture.item("resultText");
        auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        CHECK(!document->isUndoRedoEnabled());
        CHECK(editor->setProperty("cursorPosition", 0));
        fixture.key(Qt::Key_A, Qt::NoModifier, "a");
        const auto typed = fixture.file().current();
        CHECK(typed.id == initial.id + 1 && typed.draft.text == "alaunch \xf0\x9f\x98\x80\n");
        fixture.checkRecovered(typed.draft);
        fixture.select(8, 10);
        fixture.key(Qt::Key_E, Qt::NoModifier, QString::fromUtf8("\xc3\xa9"));
        const auto replaced = fixture.file().current();
        CHECK(replaced.id == typed.id + 1 && replaced.draft.text == "alaunch \xc3\xa9\n");
        CHECK(replaced.draft.decisions == initial.draft.decisions
              && fixture.text().toStdString() == replaced.draft.text);
        fixture.checkRecovered(replaced.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == typed.draft && fixture.text().toStdString() == typed.draft.text);
        fixture.checkRecovered(typed.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == initial.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft == typed.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft == replaced.draft && !document->isUndoAvailable());
        fixture.checkRecovered(replaced.draft);
    });
    scenes.run("shared edit actions follow selection and clipboard state in both controls", [] {
        Fixture fixture(test_support::choiceFile("left\n", "right\n", "original\n"));
        auto *actions = fixture.object("resultEditActions");
        const auto action = [&](const char *name) {
            auto *result = actions->property(name).value<QObject *>();
            CHECK(result);
            return result;
        };
        const auto checkEnabled = [&](const char *name, const char *button, const char *entry, bool enabled) {
            auto *shared = action(name);
            CHECK(shared->property("enabled").toBool() == enabled);
            CHECK(fixture.toolbarEnabled(button) == enabled);
            CHECK(fixture.object(entry)->property("action").value<QObject *>() == shared);
            CHECK(fixture.object(entry)->property("enabled").toBool() == enabled);
        };
        const test_support::ScopedClipboard clipboard;
        QGuiApplication::clipboard()->setText("clipboard");
        CHECK(fixture.controller.editor()->editable());
        CHECK(!fixture.item("resultText")->property("readOnly").toBool());
        checkEnabled("copy", "copyResult", "resultMenuCopy", false);
        checkEnabled("paste", "pasteResult", "resultMenuPaste", true);
        fixture.key(Qt::Key_C, Qt::ControlModifier);
        CHECK(QGuiApplication::clipboard()->text() == "clipboard");
        fixture.select(0, 4);
        checkEnabled("copy", "copyResult", "resultMenuCopy", true);
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "open"));
        settle([&] { return fixture.object("resultContextMenu")->property("opened").toBool(); });
        CHECK(fixture.item("resultMenuCopy")->isEnabled());
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "close"));
        settle([&] { return !fixture.object("resultContextMenu")->property("visible").toBool(); });
        fixture.key(Qt::Key_C, Qt::ControlModifier);
        CHECK(QGuiApplication::clipboard()->text() == "orig");
        fixture.select(0, 0);
        checkEnabled("copy", "copyResult", "resultMenuCopy", false);
        QGuiApplication::clipboard()->clear();
        checkEnabled("paste", "pasteResult", "resultMenuPaste", false);
        CHECK(fixture.controller.store()->appendFile(
            FileSession("outside", "diagram.png", {}, {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(fixture.controller.selection()->select("outside"));
        CHECK(!fixture.controller.editor()->editable());
        CHECK(fixture.item("resultText")->property("readOnly").toBool());
        CHECK(!action("paste")->property("enabled").toBool());
        CHECK(fixture.controller.selection()->select("choices"));
        QGuiApplication::clipboard()->setText("clipboard");
        checkEnabled("paste", "pasteResult", "resultMenuPaste", true);
    });
    scenes.run("the result menu opens from a right click, the Menu key and Shift+F10", [] {
        Fixture fixture;
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
        Fixture fixture(test_support::choiceFile("left\n", "right\n", "line\xe2\x80\xa8separator\n"));
        const auto before = fixture.file().current();
        const test_support::ScopedClipboard clipboard;
        QGuiApplication::clipboard()->setText("replacement");
        CHECK(!fixture.controller.editor()->editable());
        CHECK(fixture.item("resultText")->property("readOnly").toBool());
        CHECK(!fixture.toolbarEnabled("pasteResult"));
        CHECK(!fixture.object("resultMenuPaste")->property("enabled").toBool());
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "open"));
        settle([&] { return fixture.object("resultContextMenu")->property("opened").toBool(); });
        CHECK(!fixture.item("resultMenuPaste")->isEnabled());
        CHECK(QMetaObject::invokeMethod(fixture.object("resultContextMenu"), "close"));
        settle([&] { return !fixture.object("resultContextMenu")->property("visible").toBool(); });
        CHECK(!fixture.controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Paste));
        fixture.key(Qt::Key_V, Qt::ControlModifier);
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
    });
    scenes.run("clipboard menus and shortcuts preserve atomic history and recovery", [] {
        Fixture fixture(test_support::choiceFile("left\n", "right\n", "original\n"));
        const auto initial = fixture.file().current();
        const test_support::ScopedClipboard clipboard;
        CHECK(fixture.controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::SelectAll));
        CHECK(fixture.controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Copy));
        CHECK(QGuiApplication::clipboard()->text() == "original\n" && fixture.file().current().id == initial.id);
        QGuiApplication::clipboard()->setText(QString::fromUtf8("paste\t\xf0\x9f\x98\x80\n"));
        CHECK(fixture.controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Paste));
        const auto pasted = fixture.file().current();
        CHECK(pasted.id == initial.id + 1 && pasted.draft.text == "paste\t\xf0\x9f\x98\x80\n");
        fixture.checkRecovered(pasted.draft);
        fixture.select(0, 5);
        fixture.key(Qt::Key_C, Qt::ControlModifier);
        CHECK(QGuiApplication::clipboard()->text() == "paste" && fixture.file().current().id == pasted.id);
        CHECK(fixture.controller.editor()->clipboard(ResultEditorAdapter::ClipboardAction::Cut));
        const auto cut = fixture.file().current();
        CHECK(cut.id == pasted.id + 1 && cut.draft.text == "\t\xf0\x9f\x98\x80\n");
        fixture.checkRecovered(cut.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == pasted.draft);
        fixture.select(0, 5);
        QGuiApplication::clipboard()->setText("keyboard paste");
        fixture.key(Qt::Key_V, Qt::ControlModifier);
        const auto keyboard = fixture.file().current();
        CHECK(keyboard.draft.text == "keyboard paste\t\xf0\x9f\x98\x80\n");
        fixture.checkRecovered(keyboard.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == pasted.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(fixture.file().current().draft == keyboard.draft);
        fixture.checkRecovered(keyboard.draft);
    });
    scenes.run("a file click refused while composing leaves the list on the selected row", [] {
        Fixture fixture;
        auto captured = *test_support::batchFile().snapshots();
        captured.path = "other.txt";
        CHECK(fixture.controller.store()->appendFile(FileSession(
            "other", captured.path, {captured.launchResult.text.utf8, {}, 0}, {.snapshots = captured, .initializeChoices = true})));
        auto *list = fixture.item("fileList");
        auto *other = test_support::visualButton(
            list, [](QQuickItem *item) { return item->property("fileId").toString() == QStringLiteral("other"); });
        CHECK(other);
        auto *editor = fixture.item("resultText");
        QInputMethodEvent preedit(QString::fromUtf8("\xe3\x81\x8b"), {});
        QCoreApplication::sendEvent(editor, &preedit);
        CHECK(editor->property("inputMethodComposing").toBool());
        CHECK(QMetaObject::invokeMethod(other, "clicked"));
        CHECK(fixture.controller.selection()->fileId() == QStringLiteral("choices"));
        CHECK(list->property("currentIndex").toInt() == fixture.controller.selection()->row());
        QInputMethodEvent commit;
        QCoreApplication::sendEvent(editor, &commit);
        CHECK(QMetaObject::invokeMethod(other, "clicked"));
        CHECK(fixture.controller.selection()->fileId() == QStringLiteral("other"));
        CHECK(list->property("currentIndex").toInt() == fixture.controller.selection()->row());
    });
    scenes.run("an input-method preedit stays pending and its commit is one recovered command", [] {
        Fixture fixture(test_support::choiceFile());
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
        fixture.checkRecovered(chosen.draft);
        fixture.key(Qt::Key_Z, Qt::ControlModifier);
        CHECK(fixture.file().current().draft == before.draft);
    });
    for (const auto &[scopeName, scope] :
         {std::pair{QStringLiteral("left"), ChoicePreviewFlow::NonConflicting::LeftChanges},
          std::pair{QStringLiteral("right"), ChoicePreviewFlow::NonConflicting::RightChanges},
          std::pair{QStringLiteral("both"), ChoicePreviewFlow::NonConflicting::BothChanges}}) {
        scenes.run("batch preview identifies its file and source scope without changing another file "
                       + scopeName.toStdString(),
                   [=] {
                       Fixture fixture;
                       auto captured = *test_support::batchFile().snapshots();
                       captured.path = "other.txt";
                       CHECK(fixture.controller.store()->appendFile(
                           FileSession("other", captured.path, {captured.launchResult.text.utf8, {}, 0}, {.snapshots = captured, .initializeChoices = true})));
                       const auto other = fixture.controller.store()->file("other")->current();
                       CHECK(fixture.controller.choices()->previewNonConflicting("choices", "1", scope));
                       const auto preview = fixture.controller.choices()->nonConflictingPreview();
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
                       CHECK(fixture.controller.choices()->applyNonConflicting());
                       CHECK(fixture.controller.store()->file("other")->current().id == other.id);
                       CHECK(fixture.controller.store()->file("other")->current().draft == other.draft);
                       const auto replay =
                           replayRecovery(storage::RecoveryStore(fixture.controller.store()->recoveryDirectory()));
                       CHECK(replay.succeeded && replay.files.size() == 2);
                       for (const auto &file : replay.files)
                           CHECK(file.current().draft == fixture.controller.store()->file(file.id())->current().draft);
                   });
    }
    for (const int operation : {0, 1, 2}) {
        scenes.run(
            std::string("confirmation, cancellation and a stale application preserve the draft: ")
                + (operation == 0       ? "combination"
                       : operation == 1 ? "file batch"
                                        : "whole file"),
            [=] {
                Fixture fixture(operation == 1 ? test_support::batchFile() : test_support::choiceFile());
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
                };
                const auto apply = [&] {
                    return operation == 0 ? fixture.controller.choices()->applyChoice(true)
                        : operation == 1  ? fixture.controller.choices()->applyNonConflicting()
                                          : fixture.controller.choices()->applyWholeFile();
                };
                const auto *dialogName = operation == 0 ? "replacementDialog"
                    : operation == 1                    ? "nonConflictingDialog"
                                                        : "wholeFileDialog";
                open();
                auto *dialog = fixture.object(dialogName);
                CHECK(dialog->property("visible").toBool());
                CHECK(QMetaObject::invokeMethod(dialog, "reject"));
                CHECK(fixture.controller.choices()->choicePreview().isEmpty()
                      && fixture.controller.choices()->nonConflictingPreview().isEmpty()
                      && fixture.controller.choices()->wholeFilePreview().isEmpty());
                CHECK(!apply() && fixture.file().current().id == before.id
                      && fixture.file().current().draft == before.draft);
                fixture.checkRecovered(before.draft);
                open();
                CHECK(fixture.controller.commands()->editResult("choices", QString::number(before.id), 0, 0,
                                                                "newer manual edit\n"));
                const auto newer = fixture.file().current();
                // A refused confirmation keeps the dialog open and says why.
                CHECK(QMetaObject::invokeMethod(dialog, "confirm"));
                CHECK(dialog->property("visible").toBool() && dialog->property("refused").toBool());
                CHECK(dialog->property("refusalMessage").toString() == fixture.controller.feedback()->message()
                      && !fixture.controller.feedback()->message().isEmpty());
                CHECK(!apply() && fixture.file().current().id == newer.id
                      && fixture.file().current().draft == newer.draft);
                fixture.checkRecovered(newer.draft);
                CHECK(QMetaObject::invokeMethod(dialog, "reject"));
            });
    }
    scenes.run("Save and discard controls keep the app baseline and retry a failed Save", [] {
        Fixture fixture;
        auto saved = fixture.file().current().draft;
        saved.text += "saved app draft\n";
        saved.decisions["kept"] = {DecisionValue::Accepted, 4};
        CHECK(fixture.controller.store()->replaceDraft("choices", fixture.file().current().id, saved)
              == Transition::Applied);
        settle([&] { return fixture.controller.selectedFile()->unsaved(); });
        fixture.click("saveDraft");
        CHECK(fixture.controller.feedback()->message() == "Draft saved in Branch Truce; repository file unchanged.");
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
            const auto replay = replayRecovery(storage::RecoveryStore(fixture.controller.store()->recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.front().publication().checkpoint()
                  && replay.files.front().revision(*replay.files.front().publication().checkpoint())->draft == saved);
            CHECK(!replay.files.front().publication().written() && !replay.files.front().publication().staged());
        }
        auto discarded = saved;
        discarded.text += "discarded edit\n";
        discarded.decisions["kept"] = {DecisionValue::Ignored, {}};
        CHECK(fixture.controller.store()->replaceDraft("choices", fixture.file().current().id, discarded)
              == Transition::Applied);
        settle([&] { return fixture.controller.selectedFile()->unsaved(); });
        CHECK(fixture.file().revision(*fixture.file().publication().checkpoint())->draft == saved);
        fixture.click("discardUnsaved");
        CHECK(fixture.file().current().draft == saved && fixture.file().current().draft != discarded);
        CHECK(fixture.file().current().draft.decisions.at("kept").value == DecisionValue::Accepted);
        CHECK(fixture.text().toStdString() == saved.text);
        CHECK(!fixture.file().facts().unsaved && !fixture.file().publication().written()
              && !fixture.file().publication().staged());
        const auto replay = replayRecovery(storage::RecoveryStore(fixture.controller.store()->recoveryDirectory()));
        CHECK(replay.succeeded && replay.files.front().current().draft == saved);
        CHECK(replay.files.front().current().draft != discarded && !replay.files.front().publication().written()
              && !replay.files.front().publication().staged());

        CHECK(fixture.controller.commands()->editResult("choices", QString::number(fixture.file().current().id),
                                                        static_cast<int>(QString::fromStdString(saved.text).size()),
                                                        static_cast<int>(QString::fromStdString(saved.text).size()),
                                                        "retry me\n"));
        const auto edited = fixture.file().current().draft;
        {
            const test_support::ArmedFault fault(*fixture.faults, test_support::SyncFaults::Point::JournalSync);
            fixture.click("saveDraft");
        }
        settle([&] {
            return fixture.controller.selectedFile()->recoveryState() == FileStates::RecoveryState::SaveFailed;
        });
        CHECK(fixture.file().current().draft == edited && fixture.file().facts().unsaved);
        CHECK(fixture.file().revision(*fixture.file().publication().checkpoint())->draft == saved);
        CHECK(fixture.text().toStdString() == edited.text);
        settle([&] { return fixture.item("commandMessage")->isVisible(); });
        CHECK(fixture.item("commandMessage")->property("text").toString().contains("Draft save failed"));
        CHECK(fixture.controller.feedback()->message() == "Draft save failed. The edited result is still available.");
        CHECK(fixture.item("retrySave")->isVisible());
        CHECK(!fixture.item("saveDraft")->isEnabled());
        CHECK(!fixture.toolbarEnabled("discardUnsaved"));
        CHECK(!fixture.toolbarEnabled("undoCommand"));
        CHECK(!fixture.file().publication().written() && !fixture.file().publication().staged());
        fixture.click("retrySave");
        settle([&] { return !fixture.controller.selectedFile()->unsaved(); });
        CHECK(fixture.file().publication().checkpoint()
              && fixture.file().revision(*fixture.file().publication().checkpoint())->draft == edited);
        CHECK(!fixture.file().facts().unsaved && !fixture.item("retrySave")->isVisible());
        CHECK(fixture.controller.selectedFile()->status() == "Draft saved in Branch Truce; repository file unchanged");
        CHECK(!fixture.file().publication().written() && !fixture.file().publication().staged());
    });
}
