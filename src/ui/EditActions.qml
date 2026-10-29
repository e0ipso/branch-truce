// One set of result actions, shared by the toolbar, context menu and keyboard.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQml
import org.kde.kirigami as Kirigami

QtObject {
    id: editActions
    objectName: "resultEditActions"
    required property ApplicationController controller
    required property SelectedFile file
    required property bool composing
    function trigger(name: string): void {
        const action = ({undo: undo, redo: redo, copy: copy, paste: paste})[name]
        if (action && action.enabled)
            action.trigger()
    }
    // Native TextArea key events use the same actions as the window shortcuts.
    readonly property Connections editorKeys: Connections {
        target: editActions.controller
        function onResultEditActionRequested(name: string): void {
            editActions.trigger(name)
        }
    }
    readonly property NamedAction undo: NamedAction {
        controlName: "undoCommand"
        text: qsTr("Undo")
        tooltip: qsTr("Undo the last edit or choice. Typing and choices share one history.")
        icon.name: "edit-undo"
        iconOnly: true
        readonly property var bindings: StandardKeyBindings.sequences(StandardKey.Undo)
        shortcut: bindings.length ? bindings[0] : ""
        alternateSequences: bindings.slice(1)
        enabled: !!editActions.controller.resultEditable && !!editActions.file.canUndo && !editActions.composing
        onTriggered: editActions.controller.undo(editActions.controller.selectedFileId, editActions.file.currentRevision)
    }
    readonly property NamedAction redo: NamedAction {
        controlName: "redoCommand"
        text: qsTr("Redo")
        tooltip: qsTr("Redo the last undone edit or choice. Typing and choices share one history.")
        icon.name: "edit-redo"
        iconOnly: true
        readonly property var bindings: StandardKeyBindings.sequences(StandardKey.Redo)
        shortcut: bindings.length ? bindings[0] : ""
        alternateSequences: bindings.slice(1)
        enabled: !!editActions.controller.resultEditable && !!editActions.file.canRedo && !editActions.composing
        onTriggered: editActions.controller.redo(editActions.controller.selectedFileId, editActions.file.currentRevision)
    }
    readonly property NamedAction copy: NamedAction {
        controlName: "copyResult"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: qsTr("Copy")
        icon.name: "edit-copy"
        iconOnly: true
        readonly property var bindings: StandardKeyBindings.sequences(StandardKey.Copy)
        shortcut: bindings.length ? bindings[0] : ""
        alternateSequences: bindings.slice(1)
        enabled: !!editActions.controller.resultHasSelection
        onTriggered: editActions.controller.resultClipboard(ResultEditorAdapter.Copy)
    }
    readonly property NamedAction paste: NamedAction {
        controlName: "pasteResult"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: qsTr("Paste")
        icon.name: "edit-paste"
        iconOnly: true
        readonly property var bindings: StandardKeyBindings.sequences(StandardKey.Paste)
        shortcut: bindings.length ? bindings[0] : ""
        alternateSequences: bindings.slice(1)
        enabled: !!editActions.controller.resultEditable && !!editActions.controller.resultCanPaste
            && !editActions.composing
        onTriggered: editActions.controller.resultClipboard(ResultEditorAdapter.Paste)
    }
}
