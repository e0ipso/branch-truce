// The editable result's context menu: undo and redo for the file, then the clipboard actions.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Menu {
    id: resultMenu
    required property var controller
    required property var file
    required property TextArea editor
    MenuItem {
        text: qsTr("Undo")
        enabled: !!resultMenu.file.canEdit && !!resultMenu.file.canUndo && !resultMenu.editor.inputMethodComposing
        onTriggered: resultMenu.controller.undo(resultMenu.controller.selectedFileId, resultMenu.file.currentRevision)
    }
    MenuItem {
        text: qsTr("Redo")
        enabled: !!resultMenu.file.canEdit && !!resultMenu.file.canRedo && !resultMenu.editor.inputMethodComposing
        onTriggered: resultMenu.controller.redo(resultMenu.controller.selectedFileId, resultMenu.file.currentRevision)
    }
    MenuSeparator {}
    MenuItem {
        text: qsTr("Cut")
        enabled: !resultMenu.editor.readOnly && resultMenu.editor.selectedText.length > 0
            && !resultMenu.editor.inputMethodComposing
        onTriggered: resultMenu.controller.resultClipboard("cut")
    }
    MenuItem {
        text: qsTr("Copy")
        enabled: resultMenu.editor.selectedText.length > 0
        onTriggered: resultMenu.controller.resultClipboard("copy")
    }
    MenuItem {
        text: qsTr("Paste")
        enabled: !resultMenu.editor.readOnly && resultMenu.editor.canPaste && !resultMenu.editor.inputMethodComposing
        onTriggered: resultMenu.controller.resultClipboard("paste")
    }
    MenuItem {
        text: qsTr("Select all")
        onTriggered: resultMenu.controller.resultClipboard("selectAll")
    }
}
