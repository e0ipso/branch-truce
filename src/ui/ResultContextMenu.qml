// The editable result's context menu: undo and redo for the file, then the clipboard actions.
// The toolbar and window shortcuts use these same action instances.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls

Menu {
    id: resultMenu
    objectName: "resultContextMenu"
    required property ApplicationController controller
    required property TextArea editor
    required property EditActions edits
    MenuItem {
        objectName: "resultMenuUndo"
        action: resultMenu.edits.undo
    }
    MenuItem {
        objectName: "resultMenuRedo"
        action: resultMenu.edits.redo
    }
    MenuSeparator {}
    MenuItem {
        objectName: "resultMenuCut"
        text: qsTr("Cut")
        icon.name: "edit-cut"
        enabled: !!resultMenu.controller.resultEditable && !!resultMenu.controller.resultHasSelection
            && !resultMenu.editor.inputMethodComposing
        onTriggered: resultMenu.controller.resultClipboard(ResultEditorAdapter.Cut)
    }
    MenuItem {
        objectName: "resultMenuCopy"
        action: resultMenu.edits.copy
    }
    MenuItem {
        objectName: "resultMenuPaste"
        action: resultMenu.edits.paste
    }
    MenuItem {
        objectName: "resultMenuSelectAll"
        text: qsTr("Select all")
        icon.name: "edit-select-all"
        onTriggered: resultMenu.controller.resultClipboard(ResultEditorAdapter.SelectAll)
    }
}
