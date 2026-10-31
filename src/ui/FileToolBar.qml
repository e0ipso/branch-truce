// The selected file's toolbar: Save, Mark resolved, editing, source choices and view toggles.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: fileToolBar
    required property ApplicationController controller
    required property SelectedFile selectedFile
    // True while an input method composes text that the result has not acknowledged.
    required property bool composing
    // Whether the region choices show in the source panes' gutters.
    readonly property alias showSourceChoices: sourceChoices.checked
    // Whether the result shows the conflict-marker lines it folds.
    property alias showMarkers: markersToggle.checked
    property alias wrapLines: wrapAction.checked
    property alias showWhitespace: whitespaceAction.checked
    property alias showEarlier: earlierAction.checked
    signal searchRequested()
    required property EditActions edits
    signal nonConflictingChosen(scope: int)
    signal wholeFileChosen(side: int)
    signal discardToStagedChosen()
    Layout.fillWidth: true
    readonly property Kirigami.Action editSeparator: Kirigami.Action { separator: true }
    readonly property Kirigami.Action readingSeparator: Kirigami.Action { separator: true }
    readonly property NamedAction saveAction: NamedAction {
        controlName: "saveDraft"
        text: qsTr("Save")
        icon.name: "document-save"
        displayHint: Kirigami.DisplayHint.KeepVisible
        enabled: !!fileToolBar.selectedFile.canEdit && !!fileToolBar.selectedFile.unsaved && !fileToolBar.composing
        onTriggered: fileToolBar.controller.saveDraft(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision)
    }
    readonly property NamedAction resolveAction: NamedAction {
        controlName: "markResolved"
        flat: false
        text: qsTr("Mark resolved…")
        icon.name: "dialog-ok-apply"
        displayHint: Kirigami.DisplayHint.KeepVisible
        enabled: !!fileToolBar.selectedFile.canEdit && !fileToolBar.composing && !fileToolBar.controller.resolutionBusy
        onTriggered: fileToolBar.controller.markResolved(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision, false)
    }
    readonly property NamedAction discardAction: NamedAction {
        controlName: "discardUnsaved"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: qsTr("Discard")
        tooltip: qsTr("Discard unsaved edits")
        icon.name: "document-revert"
        enabled: !!fileToolBar.selectedFile.canEdit && !!fileToolBar.selectedFile.unsaved && !fileToolBar.composing
        onTriggered: fileToolBar.controller.discardUnsaved(fileToolBar.controller.selectedFileId, fileToolBar.selectedFile.currentRevision)
    }
    // Only while the draft is newer than the result staged in Git. It asks before the
    // draft is lost, so it stays in the overflow menu and the action carries the name.
    readonly property Kirigami.Action discardStagedAction: Kirigami.Action {
        objectName: "discardToStaged"
        text: qsTr("Discard draft and use staged result")
        tooltip: qsTr("Drop the newer draft and make the result staged in Git current again")
        icon.name: "document-revert"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        visible: !!fileToolBar.selectedFile.progress && !!fileToolBar.selectedFile.progress.earlierResolved
        enabled: !!fileToolBar.selectedFile.canEdit && !fileToolBar.composing && !fileToolBar.controller.resolutionBusy
        onTriggered: fileToolBar.discardToStagedChosen()
    }
    readonly property MenuAction choicesAction: MenuAction {
        id: fileChoices
        controlName: "sourceChoicesMenu"
        displayHint: Kirigami.DisplayHint.AlwaysHide
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
            onTriggered: fileToolBar.nonConflictingChosen(ChoicePreviewFlow.LeftChanges)
        }
        MenuEntry {
            controlName: "nonConflictingRight"
            text: qsTr("Preview non-conflicting Right")
            icon.name: "arrow-right"
            enabled: fileChoices.enabled
            onTriggered: fileToolBar.nonConflictingChosen(ChoicePreviewFlow.RightChanges)
        }
        MenuEntry {
            controlName: "nonConflictingBoth"
            text: qsTr("Preview non-conflicting Both")
            icon.name: "merge"
            enabled: fileChoices.enabled
            onTriggered: fileToolBar.nonConflictingChosen(ChoicePreviewFlow.BothChanges)
        }
        MenuEntry {
            controlName: "wholeFileLeft"
            text: qsTr("Preview whole file from Left")
            icon.name: "arrow-left-double"
            enabled: fileChoices.enabled
            onTriggered: fileToolBar.wholeFileChosen(ChoicePreviewFlow.LeftSide)
        }
        MenuEntry {
            controlName: "wholeFileRight"
            text: qsTr("Preview whole file from Right")
            icon.name: "arrow-right-double"
            enabled: fileChoices.enabled
            onTriggered: fileToolBar.wholeFileChosen(ChoicePreviewFlow.RightSide)
        }
    }
    // The region choices start shown; hiding them gives the source text their column.
    readonly property NamedAction sourceChoicesAction: NamedAction {
        id: sourceChoices
        controlName: "showSourceChoices"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        enabled: fileToolBar.selectedFile.support === "supported"
        text: checked ? qsTr("Hide source choices") : qsTr("Show source choices")
        icon.name: "view-list-details"
        iconOnly: true
        toggle: true
        checked: true
    }
    readonly property NamedAction searchAction: NamedAction {
        controlName: "openPassageSearch"
        enabled: fileToolBar.selectedFile.support === "supported"
        text: qsTr("Find a passage")
        icon.name: "edit-find"
        iconOnly: true
        displayHint: Kirigami.DisplayHint.KeepVisible
        onTriggered: fileToolBar.searchRequested()
    }
    readonly property NamedAction wrapLinesAction: NamedAction {
        id: wrapAction
        controlName: "wrapLines"
        enabled: fileToolBar.selectedFile.support === "supported"
        text: qsTr("Wrap lines")
        icon.name: "format-justify-left"
        iconOnly: true
        toggle: true
        displayHint: Kirigami.DisplayHint.KeepVisible
    }
    readonly property NamedAction whitespaceViewAction: NamedAction {
        id: whitespaceAction
        controlName: "showWhitespace"
        enabled: fileToolBar.selectedFile.support === "supported"
        text: qsTr("Show spaces and tabs")
        icon.name: "view-visible"
        iconOnly: true
        toggle: true
        displayHint: Kirigami.DisplayHint.KeepVisible
    }
    readonly property NamedAction earlierViewAction: NamedAction {
        id: earlierAction
        controlName: "showEarlier"
        text: enabled ? qsTr("Show common earlier version") : qsTr("Common earlier version unavailable")
        enabled: !!fileToolBar.selectedFile.earlier && !!fileToolBar.selectedFile.earlier.available
        icon.name: "view-history"
        iconOnly: true
        toggle: true
        displayHint: Kirigami.DisplayHint.KeepVisible
    }
    // A view control that folds or shows marker lines and changes no
    // text, decision, history or progress. It is always in the overflow
    // menu, as a checkable menu item, so the action carries the name.
    readonly property Kirigami.Action markersAction: Kirigami.Action {
        id: markersToggle
        objectName: "showConflictMarkers"
        enabled: fileToolBar.selectedFile.support === "supported"
        text: qsTr("Show conflict markers")
        tooltip: qsTr("Show the conflict-marker lines that the result folds. The text keeps them either way.")
        icon.name: "view-visible"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        checkable: true
    }
    Kirigami.Icon {
        objectName: "selectedTypeIcon"
        source: fileToolBar.selectedFile.mimeIcon || "unknown"
        Layout.preferredWidth: Kirigami.Units.iconSizes.small
        Layout.preferredHeight: Kirigami.Units.iconSizes.small
    }
    Kirigami.Heading {
        id: fileHeading
        level: 5
        font.weight: Font.DemiBold
        Layout.fillWidth: true
        Layout.minimumWidth: Kirigami.Units.gridUnit * 4
        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
        text: " "
        Item {
            id: selectedPathLabel
            objectName: "selectedPath"
            anchors.fill: parent
            readonly property string text: fileToolBar.selectedFile.path || ""
            readonly property string directory: text.slice(0, text.lastIndexOf("/") + 1)
            readonly property string fileName: text.slice(text.lastIndexOf("/") + 1)
            Accessible.role: Accessible.StaticText
            Accessible.name: text
            RowLayout {
                anchors.fill: parent
                spacing: 0
                PlainLabel {
                    visible: selectedPathLabel.directory.length > 0
                    Layout.maximumWidth: parent.width / 2
                    text: selectedPathLabel.directory
                    elide: Text.ElideLeft
                    font: Kirigami.Theme.defaultFont
                    color: Kirigami.Theme.disabledTextColor
                }
                PlainLabel {
                    Layout.fillWidth: true
                    text: selectedPathLabel.fileName
                    elide: Text.ElideMiddle
                    font: fileHeading.font
                }
            }
            HoverHandler { id: pathHover }
            ToolTip.text: text
            ToolTip.visible: pathHover.hovered
            ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
    Kirigami.ActionToolBar {
        objectName: "fileToolBar"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        // The path gets spare width; actions keep their native width and can overflow.
        Layout.maximumWidth: implicitWidth
        // Keep one native control row with breathing room shared by the list header.
        Layout.minimumHeight: implicitHeight + Kirigami.Units.mediumSpacing
        actions: {
            return [
                fileToolBar.saveAction,
                fileToolBar.resolveAction,
                fileToolBar.editSeparator,
                fileToolBar.discardAction,
                fileToolBar.discardStagedAction,
                fileToolBar.edits.undo,
                fileToolBar.edits.redo,
                fileToolBar.edits.copy,
                fileToolBar.edits.paste,
                fileToolBar.readingSeparator,
                fileToolBar.choicesAction,
                fileToolBar.sourceChoicesAction,
                fileToolBar.searchAction,
                fileToolBar.wrapLinesAction,
                fileToolBar.whitespaceViewAction,
                fileToolBar.earlierViewAction,
                fileToolBar.markersAction
            ]
        }
    }
    PlainLabel {
        visible: fileToolBar.composing
        text: qsTr("Composing input · not acknowledged")
    }
}
