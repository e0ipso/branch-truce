pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window
    required property var controller
    readonly property var selectedFile: controller.selectedFile
    readonly property bool supportedSelection: selectedFile.support === "supported"
    function choose(target: string, action: string): void {
        if (!controller.previewChoice(controller.selectedFileId, selectedFile.currentRevision, target, action))
            return;
        if (controller.commandPreview.confirmationRequired)
            replacementDialog.open();
        else
            controller.applyChoice(false);
    }
    function chooseNonConflicting(scope: string): void {
        if (controller.previewNonConflicting(controller.selectedFileId, selectedFile.currentRevision, scope))
            nonConflictingDialog.open();
    }
    function chooseWholeFile(side: string): void {
        if (controller.previewWholeFile(controller.selectedFileId, selectedFile.currentRevision, side))
            wholeFileDialog.open();
    }
    function progressSummary(progress): string {
        if (!progress)
            return ""
        const parts = [
            qsTr("Undecided: %1").arg(progress.undecided),
            progress.saved ? qsTr("Saved") : qsTr("Not saved"),
            progress.reviewed ? qsTr("Reviewed") : qsTr("Not reviewed"),
            progress.resolved ? qsTr("Resolved") : qsTr("Not resolved")
        ]
        if (progress.earlierResolved)
            parts.push(qsTr("Earlier result staged; current draft not applied"))
        return parts.join(" · ")
    }
    // Closing the window is an ordinary finish action: with work outstanding it
    // opens the handoff instead, where leaving is a deliberate choice.
    onClosing: close => {
        if (window.controller.sessionOutcome.length > 0 || window.controller.files.totalFiles === 0
                || window.controller.handoff.clean)
            return;
        close.accepted = false;
        handoffDialog.open();
    }
    Dialog {
        id: handoffDialog
        objectName: "handoffDialog"
        readonly property var summary: window.controller.handoff
        title: qsTr("Session handoff")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 760)
        height: Math.min(window.height - 40, 640)
        standardButtons: Dialog.Close
        ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: Kirigami.Units.smallSpacing
                Kirigami.InlineMessage {
                    objectName: "handoffOutcome"
                    Layout.fillWidth: true
                    visible: true
                    type: handoffDialog.summary.clean ? Kirigami.MessageType.Positive : Kirigami.MessageType.Warning
                    text: handoffDialog.summary.outcome || ""
                }
                Repeater {
                    model: handoffDialog.summary.sessionBlockers || []
                    delegate: Label {
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                }
                Repeater {
                    model: handoffDialog.summary.groups || []
                    delegate: ColumnLayout {
                        id: handoffGroup
                        required property var modelData
                        Layout.fillWidth: true
                        visible: modelData.entries.length > 0
                        Kirigami.Heading {
                            Layout.fillWidth: true
                            level: 4
                            text: qsTr("%1 (%2)").arg(handoffGroup.modelData.title).arg(handoffGroup.modelData.entries.length)
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            model: handoffGroup.modelData.entries
                            delegate: RowLayout {
                                id: handoffEntry
                                required property var modelData
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: window.controller.displayPath(handoffEntry.modelData.fileId) + " · "
                                        + handoffEntry.modelData.status
                                        + (handoffEntry.modelData.blockers.length > 0
                                           ? "\n" + handoffEntry.modelData.blockers.join("; ") : "")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WrapAnywhere
                                }
                                Button {
                                    objectName: "handoffRoute-" + handoffEntry.modelData.fileId
                                    text: handoffEntry.modelData.actionLabel || qsTr("Open file")
                                    onClicked: {
                                        if (window.controller.selectFile(handoffEntry.modelData.fileId))
                                            handoffDialog.close();
                                    }
                                }
                            }
                        }
                    }
                }
                Label {
                    objectName: "handoffLeaveBlocker"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: handoffDialog.summary.leaveBlocker || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "handoffFinish"
                        visible: !!handoffDialog.summary.clean
                        text: qsTr("Finish")
                        onClicked: window.controller.finishSession()
                    }
                    Button {
                        objectName: "handoffLeave"
                        visible: !handoffDialog.summary.clean
                        enabled: !!handoffDialog.summary.canLeave
                        text: qsTr("Leave with work outstanding")
                        onClicked: window.controller.leaveSession()
                    }
                }
            }
        }
    }
    Dialog {
        id: wholeFileDialog
        objectName: "wholeFileDialog"
        title: qsTr("Replace whole file from %1").arg(window.controller.wholeFilePreview.side || "")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 950)
        height: Math.min(window.height - 40, 650)
        standardButtons: Dialog.Apply | Dialog.Cancel
        onApplied: {
            window.controller.applyWholeFile();
            close();
        }
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.wholeFilePreview.path || ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.wholeFilePreview.source || ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                objectName: "wholeFileWarning"
                Layout.fillWidth: true
                text: window.controller.wholeFilePreview.detail || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            TabBar {
                id: wholeFileTabs
                Layout.fillWidth: true
                TabButton { text: qsTr("Full file") }
                TabButton { text: qsTr("Contribution decisions") }
                TabButton { text: qsTr("Text and edits replaced") }
            }
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: wholeFileTabs.currentIndex
                RowLayout {
                    SnapshotPane {
                        paneName: "wholeFileBefore"
                        paneTitle: qsTr("Full current result, including manual edits")
                        paneText: window.controller.wholeFilePreview.before || ""
                    }
                    SnapshotPane {
                        paneName: "wholeFileAfter"
                        paneTitle: qsTr("Full proposed result")
                        paneText: window.controller.wholeFilePreview.replacement || ""
                    }
                }
                ColumnLayout {
                    Label {
                        Layout.fillWidth: true
                        visible: !!window.controller.wholeFilePreview.retainedDecisions
                        text: window.controller.wholeFilePreview.retainedDecisions || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    ListView {
                        objectName: "wholeFileContributions"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: window.controller.wholeFilePreview.contributions || []
                        delegate: ColumnLayout {
                            id: wholeContribution
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: wholeContribution.modelData.scope + "\n"
                                    + wholeContribution.modelData.beforeState + " → " + wholeContribution.modelData.afterState
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            SnapshotPane {
                                Layout.preferredHeight: 120
                                paneName: "wholeContributionSource"
                                paneTitle: qsTr("Source contribution")
                                paneText: wholeContribution.modelData.sourceText
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
                ColumnLayout {
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("All differences from the selected source are replaced, including manual and pre-existing edits. Identical text stays the same.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    ListView {
                        objectName: "wholeFileDifferences"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: window.controller.wholeFilePreview.differences || []
                        delegate: ColumnLayout {
                            id: wholeDifference
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: wholeDifference.modelData.scope
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 150
                                SnapshotPane {
                                    paneName: "wholeDifferenceBefore"
                                    paneTitle: qsTr("Current text being replaced")
                                    paneText: wholeDifference.modelData.before
                                }
                                SnapshotPane {
                                    paneName: "wholeDifferenceAfter"
                                    paneTitle: qsTr("Replacement text")
                                    paneText: wholeDifference.modelData.replacement
                                }
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
            }
        }
    }
    Dialog {
        id: nonConflictingDialog
        objectName: "nonConflictingDialog"
        title: qsTr("Apply non-conflicting changes · %1").arg(window.controller.nonConflictingPreview.scope || "")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 750)
        height: Math.min(window.height - 40, 600)
        standardButtons: Dialog.Apply | Dialog.Cancel
        onApplied: {
            window.controller.applyNonConflicting();
            close();
        }
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.nonConflictingPreview.path || ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.nonConflictingPreview.summary || ""
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.nonConflictingPreview.detail || ""
                wrapMode: Text.Wrap
            }
            ListView {
                objectName: "nonConflictingChanges"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: window.controller.nonConflictingPreview.changes || []
                delegate: ColumnLayout {
                    id: batchChange
                    required property var modelData
                    width: ListView.view.width
                    Label {
                        Layout.fillWidth: true
                        text: batchChange.modelData.scope
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        SnapshotPane {
                            paneName: "batchBefore"
                            paneTitle: qsTr("Current text in this scope")
                            paneText: batchChange.modelData.before
                        }
                        SnapshotPane {
                            paneName: "batchAfter"
                            paneTitle: qsTr("Proposed text")
                            paneText: batchChange.modelData.replacement
                        }
                    }
                }
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: !!window.selectedFile.canUndo && !!window.selectedFile.canEdit && !comparison.inputMethodComposing
        onActivated: window.controller.undo(window.controller.selectedFileId, window.selectedFile.currentRevision)
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: !!window.selectedFile.canRedo && !!window.selectedFile.canEdit && !comparison.inputMethodComposing
        onActivated: window.controller.redo(window.controller.selectedFileId, window.selectedFile.currentRevision)
    }
    Dialog {
        id: replacementDialog
        objectName: "replacementDialog"
        title: window.controller.commandPreview.orderedCombination
            ? qsTr("Preview %1").arg(window.controller.commandPreview.order)
            : qsTr("Replace manual work in this scope?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 700)
        height: Math.min(window.height - 40, 600)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: window.controller.applyChoice(true)
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.commandPreview.detail || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: !!window.controller.commandPreview.orderedCombination
                text: window.controller.commandPreview.sourceScopes || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "manualReplacementNotice"
                Layout.fillWidth: true
                visible: !!window.controller.commandPreview.replacesManual
                text: qsTr("Manual or pre-existing edits in the current scope below will be replaced. Confirm to replace this work, or cancel to keep it.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                SnapshotPane {
                    paneName: "replacementBefore"
                    paneTitle: qsTr("Current text in this scope")
                    paneText: window.controller.commandPreview.before || ""
                }
                SnapshotPane {
                    paneName: "replacementAfter"
                    paneTitle: window.controller.commandPreview.orderedCombination
                        ? qsTr("Proposed text · %1").arg(window.controller.commandPreview.order)
                        : qsTr("Replacement from the selected source")
                    paneText: window.controller.commandPreview.replacement || ""
                }
            }
        }
    }
    component ConflictRow: ItemDelegate {
        id: row
        required property int index
        required property string fileId
        required property string support
        required property string supportDetail
        required property string status
        required property bool unsaved
        required property var progress
        width: ListView.view.width
        text: window.controller.displayPath(fileId) + (support === "supported" ? ""
            : support === "unreadable" ? qsTr(" · load error · outside app") : qsTr(" · outside app"))
        highlighted: fileId === window.controller.selectedFileId
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: row.text
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                visible: row.support !== "supported"
                text: row.supportDetail
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: row.status + "\n" + (row.unsaved ? qsTr("Unsaved draft") : qsTr("Manual Save checkpoint retained"))
                    + "\n" + window.progressSummary(row.progress)
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
        }
    }
    component SnapshotPane: ColumnLayout {
        id: pane
        required property string paneName
        required property string paneTitle
        required property string paneText
        Layout.fillWidth: true
        Layout.fillHeight: true
        Label {
            objectName: pane.paneName + "Label"
            Layout.fillWidth: true
            text: pane.paneTitle
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                objectName: pane.paneName + "Text"
                text: pane.paneText
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                font.family: "monospace"
                wrapMode: TextEdit.NoWrap
                Accessible.name: pane.paneTitle
            }
        }
    }
    width: 1200
    height: 850
    minimumWidth: 1000
    minimumHeight: 760
    visible: true
    title: qsTr("Branch Truce")
    pageStack.initialPage: Kirigami.Page {
        title: window.controller.operation || qsTr("Open conflict session")
        ColumnLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.largeSpacing
            Label {
                Layout.fillWidth: true
                text: window.controller.repositoryRoot
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            RowLayout {
                BusyIndicator { running: window.controller.opening; visible: running }
                Label {
                    Layout.fillWidth: true
                    text: window.controller.detail
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 files in this session · %2 resolved")
                    .arg(window.controller.files.totalFiles).arg(window.controller.files.resolvedFiles)
                visible: !window.controller.opening && window.controller.files.totalFiles > 0
            }
            Kirigami.InlineMessage {
                objectName: "inventoryWarning"
                Layout.fillWidth: true
                visible: !window.controller.opening
                    && window.controller.outsideAppFiles + window.controller.loadErrorFiles > 0
                type: Kirigami.MessageType.Warning
                text: qsTr("%1 outside app · %2 load errors. These files remain outstanding. Select a file for its reason and handle it outside Branch Truce.")
                    .arg(window.controller.outsideAppFiles).arg(window.controller.loadErrorFiles)
            }
            Flow {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                visible: !window.controller.opening && window.controller.files.totalFiles > 0
                Button {
                    objectName: "openHandoff"
                    text: qsTr("Review handoff…")
                    onClicked: handoffDialog.open()
                }
                Button {
                    objectName: "toggleFileList"
                    text: window.controller.fileListVisible ? qsTr("Hide file list") : qsTr("Show file list")
                    onClicked: window.controller.toggleFileList()
                }
                Button {
                    objectName: "previousDifference"
                    text: qsTr("Previous difference")
                    onClicked: window.controller.navigate("difference", -1)
                }
                Button {
                    objectName: "nextDifference"
                    text: qsTr("Next difference")
                    onClicked: window.controller.navigate("difference", 1)
                }
                Button {
                    objectName: "previousUndecided"
                    text: qsTr("Previous undecided")
                    onClicked: window.controller.navigate("undecided", -1)
                }
                Button {
                    objectName: "nextUndecided"
                    text: qsTr("Next undecided")
                    onClicked: window.controller.navigate("undecided", 1)
                }
                Button {
                    objectName: "previousUnfinished"
                    text: qsTr("Previous unfinished file")
                    onClicked: window.controller.navigate("unfinished", -1)
                }
                Button {
                    objectName: "nextUnfinished"
                    text: qsTr("Next unfinished file")
                    onClicked: window.controller.navigate("unfinished", 1)
                }
                Button {
                    objectName: "previousSavedOrReviewed"
                    text: qsTr("Previous saved or reviewed")
                    onClicked: window.controller.navigate("saved-or-reviewed", -1)
                }
                Button {
                    objectName: "nextSavedOrReviewed"
                    text: qsTr("Next saved or reviewed")
                    onClicked: window.controller.navigate("saved-or-reviewed", 1)
                }
            }
            Label {
                objectName: "navigationTarget"
                Layout.fillWidth: true
                visible: !window.controller.opening && (window.controller.navigationTarget.label || "").length > 0
                text: window.controller.navigationTarget.label || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !window.controller.opening && window.controller.files.totalFiles > 0
                ListView {
                    id: fileList
                    objectName: "fileList"
                    visible: window.controller.fileListVisible
                    Layout.preferredWidth: 220
                    Layout.fillHeight: true
                    clip: true
                    model: window.controller.files
                    currentIndex: 0
                    function revealSelection(): void {
                        const row = window.controller.selectedRow()
                        if (row < 0 || row >= count)
                            return
                        currentIndex = row
                        positionViewAtIndex(row, ListView.Contain)
                    }
                    Component.onCompleted: revealSelection()
                    Connections {
                        target: window.controller
                        function onSelectedFileChanged(): void {
                            fileList.revealSelection()
                        }
                    }
                    delegate: ConflictRow {
                        id: fileDelegate
                        onClicked: {
                            fileList.currentIndex = fileDelegate.index;
                            window.controller.selectFile(fileDelegate.fileId);
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Label {
                        Layout.fillWidth: true
                        objectName: "selectedPath"
                        text: window.selectedFile.path || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        objectName: "selectedProblem"
                        visible: window.controller.selectedFileId.length > 0 && !window.supportedSelection
                        text: (window.selectedFile.support === "unreadable"
                            ? qsTr("Load error · outside app · outstanding\n") : qsTr("Outside app · outstanding\n"))
                            + (window.selectedFile.supportDetail || "")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "selectedStatus"
                        Layout.fillWidth: true
                        visible: window.controller.selectedFileId.length > 0
                        text: (window.selectedFile.status || "") + "\n"
                            + (window.selectedFile.unsaved ? qsTr("Unsaved draft") : qsTr("Manual Save checkpoint retained"))
                            + "\n" + window.progressSummary(window.selectedFile.progress)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Kirigami.InlineMessage {
                        objectName: "recoveryProblem"
                        Layout.fillWidth: true
                        visible: !!window.selectedFile.recoveryError
                        type: Kirigami.MessageType.Error
                        text: window.selectedFile.recoveryError || ""
                    }
                    Label {
                        objectName: "commandMessage"
                        Layout.fillWidth: true
                        visible: window.controller.commandMessage.length > 0
                        text: window.controller.commandMessage
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "parentStagingState"
                        Layout.fillWidth: true
                        visible: window.controller.parentStagingPath.length > 0
                        text: qsTr("Written, unresolved. Waiting for Git to stage %1.").arg(window.controller.parentStagingPath)
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "recheckParentStaging"
                        visible: window.controller.parentStagingPath.length > 0
                        text: qsTr("Check Git staging")
                        enabled: !window.controller.resolutionBusy
                        onClicked: window.controller.recheckParentAcknowledgment()
                    }
                    Label {
                        objectName: "mergetoolHolding"
                        Layout.fillWidth: true
                        visible: window.controller.mergetoolWaiting
                        text: qsTr("Git is waiting on this path. Mark resolved hands it back. Leave keeps the draft and returns the original file.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "leaveMergetool"
                        visible: window.controller.mergetoolWaiting
                        enabled: !window.controller.resolutionBusy
                        text: qsTr("Leave unresolved")
                        onClicked: window.controller.leaveMergetool()
                    }
                    Label {
                        objectName: "mergetoolWait"
                        Layout.fillWidth: true
                        visible: window.controller.parentSessionActive && !window.controller.mergetoolWaiting
                                 && window.controller.parentStagingPath.length === 0 && window.supportedSelection
                        text: qsTr("Waiting for Git to launch this path. It is not resolved.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Button {
                        objectName: "endParentSession"
                        visible: window.controller.canEndParentSession
                        text: qsTr("Parent Git is finished")
                        onClicked: window.controller.endParentSession()
                    }
                    RowLayout {
                        visible: window.controller.resolutionConfirmation
                            && window.controller.resolutionFileId === window.controller.selectedFileId
                            && window.controller.resolutionRevision === window.selectedFile.currentRevision
                        Button {
                            objectName: "confirmResolution"
                            text: qsTr("Confirm resolution")
                            enabled: !window.controller.resolutionBusy && !comparison.inputMethodComposing
                            onClicked: window.controller.markResolved(window.controller.selectedFileId, window.selectedFile.currentRevision, true)
                        }
                        Button {
                            objectName: "cancelResolution"
                            text: qsTr("Cancel")
                            onClicked: window.controller.cancelResolution()
                        }
                    }
                    Button {
                        objectName: "retryRecovery"
                        visible: !!window.selectedFile.pendingInput && window.selectedFile.recoveryState !== "save-failed"
                        text: qsTr("Retry keeping pending input")
                        onClicked: window.controller.files.retryPendingRecovery(window.controller.selectedFileId)
                    }
                    Button {
                        objectName: "retrySave"
                        visible: window.selectedFile.recoveryState === "save-failed"
                        text: qsTr("Retry Save")
                        onClicked: window.controller.files.retryPendingRecovery(window.controller.selectedFileId)
                    }
                    SnapshotPane {
                        visible: !!window.selectedFile.pendingInput && window.selectedFile.recoveryState !== "save-failed"
                        paneName: "pendingInput"
                        paneTitle: qsTr("Pending input · not acknowledged")
                        paneText: window.selectedFile.pendingText || ""
                    }
                    RowLayout {
                        visible: window.supportedSelection
                        Button {
                            objectName: "saveDraft"
                            text: qsTr("Save")
                            enabled: !!window.selectedFile.canEdit && !!window.selectedFile.unsaved && !comparison.inputMethodComposing
                            onClicked: window.controller.saveDraft(window.controller.selectedFileId, window.selectedFile.currentRevision)
                        }
                        Button {
                            objectName: "markResolved"
                            text: qsTr("Mark resolved")
                            enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing && !window.controller.resolutionBusy
                            onClicked: window.controller.markResolved(window.controller.selectedFileId, window.selectedFile.currentRevision, false)
                        }
                        Button {
                            objectName: "discardUnsaved"
                            text: qsTr("Discard")
                            enabled: !!window.selectedFile.canEdit && !!window.selectedFile.unsaved && !comparison.inputMethodComposing
                            onClicked: window.controller.discardUnsaved(window.controller.selectedFileId, window.selectedFile.currentRevision)
                        }
                        Button {
                            objectName: "undoCommand"
                            text: qsTr("Undo")
                            enabled: !!window.selectedFile.canEdit && !!window.selectedFile.canUndo && !comparison.inputMethodComposing
                            onClicked: window.controller.undo(window.controller.selectedFileId, window.selectedFile.currentRevision)
                        }
                        Button {
                            objectName: "redoCommand"
                            text: qsTr("Redo")
                            enabled: !!window.selectedFile.canEdit && !!window.selectedFile.canRedo && !comparison.inputMethodComposing
                            onClicked: window.controller.redo(window.controller.selectedFileId, window.selectedFile.currentRevision)
                        }
                        Button {
                            objectName: "copyResult"
                            text: qsTr("Copy")
                            onClicked: window.controller.resultClipboard("copy")
                        }
                        Button {
                            objectName: "pasteResult"
                            text: qsTr("Paste")
                            enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing
                            onClicked: window.controller.resultClipboard("paste")
                        }
                        Button {
                            id: sourceChoices
                            objectName: "showSourceChoices"
                            text: checked ? qsTr("Hide source choices") : qsTr("Show source choices")
                            checkable: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: comparison.inputMethodComposing ? qsTr("Composing input · not acknowledged") : qsTr("Shared undo/redo")
                            elide: Text.ElideRight
                        }
                    }
                    Label {
                        objectName: "nonConflictingScope"
                        Layout.fillWidth: true
                        visible: window.supportedSelection && sourceChoices.checked
                        text: qsTr("Non-conflicting changes in %1. Preview the eligible changes before applying.")
                            .arg(window.selectedFile.path || "")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        visible: window.supportedSelection && sourceChoices.checked
                        enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing
                        Button {
                            objectName: "nonConflictingLeft"
                            text: qsTr("Preview non-conflicting Left")
                            onClicked: window.chooseNonConflicting("left")
                        }
                        Button {
                            objectName: "nonConflictingRight"
                            text: qsTr("Preview non-conflicting Right")
                            onClicked: window.chooseNonConflicting("right")
                        }
                        Button {
                            objectName: "nonConflictingBoth"
                            text: qsTr("Preview non-conflicting Both")
                            onClicked: window.chooseNonConflicting("both")
                        }
                    }
                    RowLayout {
                        visible: window.supportedSelection && sourceChoices.checked
                        enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing
                        Button {
                            objectName: "wholeFileLeft"
                            text: qsTr("Preview whole file from Left")
                            onClicked: window.chooseWholeFile("left")
                        }
                        Button {
                            objectName: "wholeFileRight"
                            text: qsTr("Preview whole file from Right")
                            onClicked: window.chooseWholeFile("right")
                        }
                    }
                    ComparisonView {
                        id: comparison
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 200
                        visible: window.supportedSelection
                        controller: window.controller
                        file: window.selectedFile
                    }
                    ListView {
                        objectName: "scopedChoices"
                        visible: window.supportedSelection && sourceChoices.checked
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 180)
                        clip: true
                        model: window.selectedFile.choices || []
                        delegate: ColumnLayout {
                            id: choiceRow
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: choiceRow.modelData.label + " · " + choiceRow.modelData.state
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: choiceRow.modelData.sourceText || qsTr("Empty source scope · deletion")
                                textFormat: Text.PlainText
                                font.family: "monospace"
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }
                            RowLayout {
                                enabled: !!window.selectedFile.canEdit && !!choiceRow.modelData.available && !comparison.inputMethodComposing
                                Button {
                                    text: qsTr("Accept →")
                                    Accessible.name: qsTr("Accept %1").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "accept")
                                }
                                Button {
                                    text: qsTr("× Ignore")
                                    Accessible.name: qsTr("Ignore %1").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "ignore")
                                }
                                Button {
                                    text: qsTr("Resolve using this side")
                                    Accessible.name: qsTr("Resolve %1 using this side and ignore its competitor").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "resolve")
                                }
                            }
                            RowLayout {
                                enabled: !!window.selectedFile.canEdit && !!choiceRow.modelData.combinationAvailable && !comparison.inputMethodComposing
                                Button {
                                    text: qsTr("Preview Left then Right")
                                    Accessible.name: qsTr("Preview both for %1, Left then Right").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "left-first")
                                }
                                Button {
                                    text: qsTr("Preview Right then Left")
                                    Accessible.name: qsTr("Preview both for %1, Right then Left").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "right-first")
                                }
                            }
                            Label {
                                visible: !choiceRow.modelData.available
                                Layout.fillWidth: true
                                text: choiceRow.modelData.detail
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: sourceChoices.checked && !!window.selectedFile.choiceNotice
                        text: window.selectedFile.choiceNotice || ""
                        wrapMode: Text.Wrap
                    }

                }
            }
            Label {
                Layout.fillWidth: true
                visible: window.controller.stage2.length > 0
                text: window.controller.stage2 + "\n" + window.controller.stage3
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.workflow
                wrapMode: Text.Wrap
            }
        }
    }
}
