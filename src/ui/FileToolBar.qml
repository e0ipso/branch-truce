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
    // Whether the region choices show in the source panes' gutters.
    readonly property alias showSourceChoices: sourceChoices.checked
    // Whether the result shows the conflict-marker lines it folds.
    property alias showMarkers: markersToggle.checked
    required property EditActions edits
    signal nonConflictingChosen(scope: string)
    signal wholeFileChosen(side: string)
    signal discardToStagedChosen()
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
            // Only while the draft is newer than the result staged in Git. It asks before the
            // draft is lost, so it stays in the overflow menu and the action carries the name.
            Kirigami.Action {
                objectName: "discardToStaged"
                text: qsTr("Discard draft and use staged result")
                tooltip: qsTr("Drop the newer draft and make the result staged in Git current again")
                icon.name: "document-revert"
                displayHint: Kirigami.DisplayHint.AlwaysHide
                visible: !!fileToolBar.selectedFile.progress && !!fileToolBar.selectedFile.progress.earlierResolved
                enabled: !!fileToolBar.selectedFile.canEdit && !fileToolBar.composing && !fileToolBar.controller.resolutionBusy
                onTriggered: fileToolBar.discardToStagedChosen()
            },
            fileToolBar.edits.undo,
            fileToolBar.edits.redo,
            fileToolBar.edits.copy,
            fileToolBar.edits.paste,
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
            // The region choices start shown; hiding them gives the source text their column.
            NamedAction {
                id: sourceChoices
                controlName: "showSourceChoices"
                text: checked ? qsTr("Hide source choices") : qsTr("Show source choices")
                icon.name: "view-list-details"
                iconOnly: true
                toggle: true
                checked: true
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
