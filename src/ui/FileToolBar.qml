// The selected file's toolbar: Save, Mark resolved, editing, source choices and view toggles.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: fileToolBar
    required property var controller
    required property var selectedFile
    // True while an input method composes text that the result has not acknowledged.
    required property bool composing
    // Whether the source choices list shows.
    readonly property alias showSourceChoices: sourceChoices.checked
    // Whether the result shows the conflict-marker lines it folds.
    property alias showMarkers: markersToggle.checked
    signal nonConflictingChosen(scope: string)
    signal wholeFileChosen(side: string)
    Layout.fillWidth: true
    Kirigami.ActionToolBar {
        objectName: "fileToolBar"
        Layout.fillWidth: true
        actions: [
            NamedAction {
                controlName: "saveDraft"
                text: qsTr("Save")
                icon.name: "document-save"
                displayHint: Kirigami.DisplayHint.KeepVisible
                enabled: !!fileToolBar.selectedFile.canEdit && !!fileToolBar.selectedFile.unsaved && !fileToolBar.composing
                onTriggered: fileToolBar.controller.saveDraft(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision)
            },
            NamedAction {
                controlName: "markResolved"
                text: qsTr("Mark resolved")
                icon.name: "dialog-ok-apply"
                displayHint: Kirigami.DisplayHint.KeepVisible
                enabled: !!fileToolBar.selectedFile.canEdit && !fileToolBar.composing && !fileToolBar.controller.resolutionBusy
                onTriggered: fileToolBar.controller.markResolved(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision, false)
            },
            NamedAction {
                controlName: "discardUnsaved"
                text: qsTr("Discard")
                tooltip: qsTr("Discard unsaved edits")
                icon.name: "document-revert"
                enabled: !!fileToolBar.selectedFile.canEdit && !!fileToolBar.selectedFile.unsaved && !fileToolBar.composing
                onTriggered: fileToolBar.controller.discardUnsaved(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision)
            },
            NamedAction {
                controlName: "undoCommand"
                text: qsTr("Undo")
                tooltip: qsTr("Undo the last edit or choice. Typing and choices share one history.")
                icon.name: "edit-undo"
                iconOnly: true
                enabled: !!fileToolBar.selectedFile.canEdit && !!fileToolBar.selectedFile.canUndo && !fileToolBar.composing
                onTriggered: fileToolBar.controller.undo(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision)
            },
            NamedAction {
                controlName: "redoCommand"
                text: qsTr("Redo")
                tooltip: qsTr("Redo the last undone edit or choice. Typing and choices share one history.")
                icon.name: "edit-redo"
                iconOnly: true
                enabled: !!fileToolBar.selectedFile.canEdit && !!fileToolBar.selectedFile.canRedo && !fileToolBar.composing
                onTriggered: fileToolBar.controller.redo(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision)
            },
            NamedAction {
                controlName: "copyResult"
                text: qsTr("Copy")
                icon.name: "edit-copy"
                iconOnly: true
                onTriggered: fileToolBar.controller.resultClipboard("copy")
            },
            NamedAction {
                controlName: "pasteResult"
                text: qsTr("Paste")
                icon.name: "edit-paste"
                iconOnly: true
                enabled: !!fileToolBar.selectedFile.canEdit && !fileToolBar.composing
                onTriggered: fileToolBar.controller.resultClipboard("paste")
            },
            MenuAction {
                id: fileChoices
                controlName: "sourceChoicesMenu"
                text: qsTr("Source choices")
                tooltip: qsTr("Preview non-conflicting changes or a whole side of %1 before applying")
                    .arg(fileToolBar.selectedFile.path || "")
                icon.name: "document-import"
                enabled: !!fileToolBar.selectedFile.canEdit && !fileToolBar.composing
                MenuEntry {
                    controlName: "nonConflictingLeft"
                    text: qsTr("Preview non-conflicting Left")
                    icon.name: "arrow-left"
                    enabled: fileChoices.enabled
                    onTriggered: fileToolBar.nonConflictingChosen("left")
                }
                MenuEntry {
                    controlName: "nonConflictingRight"
                    text: qsTr("Preview non-conflicting Right")
                    icon.name: "arrow-right"
                    enabled: fileChoices.enabled
                    onTriggered: fileToolBar.nonConflictingChosen("right")
                }
                MenuEntry {
                    controlName: "nonConflictingBoth"
                    text: qsTr("Preview non-conflicting Both")
                    icon.name: "merge"
                    enabled: fileChoices.enabled
                    onTriggered: fileToolBar.nonConflictingChosen("both")
                }
                MenuEntry {
                    controlName: "wholeFileLeft"
                    text: qsTr("Preview whole file from Left")
                    icon.name: "arrow-left-double"
                    enabled: fileChoices.enabled
                    onTriggered: fileToolBar.wholeFileChosen("left")
                }
                MenuEntry {
                    controlName: "wholeFileRight"
                    text: qsTr("Preview whole file from Right")
                    icon.name: "arrow-right-double"
                    enabled: fileChoices.enabled
                    onTriggered: fileToolBar.wholeFileChosen("right")
                }
            },
            NamedAction {
                id: sourceChoices
                controlName: "showSourceChoices"
                text: checked ? qsTr("Hide source choices") : qsTr("Show source choices")
                icon.name: "view-list-details"
                iconOnly: true
                toggle: true
            },
            // A view control that folds or shows marker lines and changes no
            // text, decision, history or progress. It is always in the overflow
            // menu, as a checkable menu item, so the action carries the name.
            Kirigami.Action {
                id: markersToggle
                objectName: "showConflictMarkers"
                text: qsTr("Show conflict markers")
                tooltip: qsTr("Show the conflict-marker lines that the result folds. The text keeps them either way.")
                icon.name: "view-visible"
                displayHint: Kirigami.DisplayHint.AlwaysHide
                checkable: true
            }
        ]
    }
    Label {
        visible: fileToolBar.composing
        text: qsTr("Composing input · not acknowledged")
        textFormat: Text.PlainText
    }
}
