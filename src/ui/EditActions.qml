// One set of result actions, shared by the toolbar, context menu and keyboard.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQml
import org.kde.kirigami as Kirigami

QtObject {
    id: editActions
    objectName: "resultEditActions"
    required property ResultEditorAdapter editor
    required property FileCommands commands
    required property SelectedFile file
    required property bool composing
    function trigger(name: string): void {
        const action = ({undo: undo, redo: redo, copy: copy, paste: paste})[name]
        if (action && action.enabled)
            action.trigger()
    }
    // Native TextArea key events use the same actions as the window shortcuts.
    readonly property Connections editorKeys: Connections {
        target: editActions.editor
        function onEditActionRequested(name: string): void {
            editActions.trigger(name)
        }
    }
    readonly property NamedAction undo: NamedAction {
        controlName: "undoCommand"
        text: qsTr("Undo")
        tooltip: qsTr("Undo the last edit or choice. Typing and choices share one history.")
        icon.name: "edit-undo"
        iconOnly: true
        standardKey: StandardKey.Undo
        enabled: !!editActions.editor.editable && !!editActions.file.canUndo && !editActions.composing
        onTriggered: editActions.commands.undo(editActions.file.fileId, editActions.file.currentRevision)
    }
    readonly property NamedAction redo: NamedAction {
        controlName: "redoCommand"
        text: qsTr("Redo")
        tooltip: qsTr("Redo the last undone edit or choice. Typing and choices share one history.")
        icon.name: "edit-redo"
        iconOnly: true
        standardKey: StandardKey.Redo
        enabled: !!editActions.editor.editable && !!editActions.file.canRedo && !editActions.composing
        onTriggered: editActions.commands.redo(editActions.file.fileId, editActions.file.currentRevision)
    }
    readonly property NamedAction copy: NamedAction {
        controlName: "copyResult"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: qsTr("Copy")
        icon.name: "edit-copy"
        iconOnly: true
        standardKey: StandardKey.Copy
        enabled: !!editActions.editor.hasSelection
        onTriggered: editActions.editor.clipboard(ResultEditorAdapter.Copy)
    }
    readonly property NamedAction paste: NamedAction {
        controlName: "pasteResult"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: qsTr("Paste")
        icon.name: "edit-paste"
        iconOnly: true
        standardKey: StandardKey.Paste
        enabled: !!editActions.editor.editable && !!editActions.editor.canPaste
            && !editActions.composing
        onTriggered: editActions.editor.clipboard(ResultEditorAdapter.Paste)
    }
}
