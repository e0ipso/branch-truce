// The selected file's notices and recovery buttons: messages, Git staging and mergetool state,
// resolution confirmation and retries for input that recovery could not keep.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models
import org.kde.kirigami as Kirigami

// An object model, not a column: a Repeater places these items straight into the work area's
// layout, which spaces them and shares its height with the pending-input pane as before. A
// wrapping column would take a spacing and a share of the height even with nothing to show.
ObjectModel {
    id: fileNotices
    required property ApplicationController controller
    required property SelectedFile selectedFile
    required property bool supportedSelection
    // True while an input method composes text that the result has not acknowledged.
    required property bool composing
    // Asks the owner to set plain text on a message and each label inside it, once they exist.
    signal plainTextRequested(item: Item)
    // Emitted once the result's cursor is on the first conflict marker, so the view can show it.
    signal firstMarkerShown()
    PlainInlineMessage {
        objectName: "recoveryProblem"
        onPlainTextRequested: item => fileNotices.plainTextRequested(item)
        Layout.fillWidth: true
        visible: !!fileNotices.selectedFile.recoveryError
        type: Kirigami.MessageType.Error
        text: fileNotices.selectedFile.recoveryError || ""
    }
    PlainInlineMessage {
        objectName: "commandMessage"
        onPlainTextRequested: item => fileNotices.plainTextRequested(item)
        type: Kirigami.MessageType.Information
        icon.name: "dialog-information"
        Layout.fillWidth: true
        visible: fileNotices.controller.commandMessage.length > 0
        text: fileNotices.controller.commandMessage
        actions: [
            Kirigami.Action {
                objectName: "showFirstMarker"
                text: qsTr("Go to line %1").arg(fileNotices.controller.markerBlocker.line || 0)
                icon.name: "go-jump"
                visible: !!fileNotices.controller.markerBlocker.line
                onTriggered: {
                    if (fileNotices.controller.showFirstMarker())
                        fileNotices.firstMarkerShown()
                }
            }
        ]
    }
    PlainInlineMessage {
        objectName: "parentStagingState"
        onPlainTextRequested: item => fileNotices.plainTextRequested(item)
        type: Kirigami.MessageType.Warning
        icon.name: "vcs-locally-modified"
        Layout.fillWidth: true
        visible: fileNotices.controller.parentStagingPath.length > 0
        text: qsTr("Written, unresolved. Waiting for Git to stage %1.").arg(fileNotices.controller.parentStagingPath)
    }
    Button {
        objectName: "recheckParentStaging"
        visible: fileNotices.controller.parentStagingPath.length > 0
        text: qsTr("Check Git staging")
        enabled: !fileNotices.controller.resolutionBusy
        onClicked: fileNotices.controller.recheckParentAcknowledgment()
    }
    PlainInlineMessage {
        objectName: "mergetoolHolding"
        onPlainTextRequested: item => fileNotices.plainTextRequested(item)
        type: Kirigami.MessageType.Information
        icon.name: "vcs-conflicting"
        Layout.fillWidth: true
        visible: fileNotices.controller.mergetoolWaiting
        text: qsTr("Git is waiting on this path. Mark resolved hands it back. Leave keeps the draft and returns the original file.")
    }
    Button {
        objectName: "leaveMergetool"
        visible: fileNotices.controller.mergetoolWaiting
        enabled: !fileNotices.controller.resolutionBusy
        text: qsTr("Leave unresolved")
        onClicked: fileNotices.controller.leaveMergetool()
    }
    // A path whose result Git already staged is not waiting for a launch.
    PlainInlineMessage {
        objectName: "mergetoolWait"
        onPlainTextRequested: item => fileNotices.plainTextRequested(item)
        type: Kirigami.MessageType.Warning
        icon.name: "chronometer"
        Layout.fillWidth: true
        visible: fileNotices.controller.parentSessionActive && !fileNotices.controller.mergetoolWaiting
                 && fileNotices.controller.parentStagingPath.length === 0 && fileNotices.supportedSelection
                 && !fileNotices.selectedFile.resolved
                 && !(fileNotices.selectedFile.progress && fileNotices.selectedFile.progress.earlierResolved)
        text: qsTr("Waiting for Git to launch this path. It is not resolved.")
    }
    Button {
        objectName: "endParentSession"
        visible: fileNotices.controller.canEndParentSession
        text: qsTr("Parent Git is finished")
        onClicked: fileNotices.controller.endParentSession()
    }
    RowLayout {
        visible: fileNotices.controller.resolutionConfirmation
            && fileNotices.controller.resolutionFileId === fileNotices.controller.selectedFileId
            && fileNotices.controller.resolutionRevision === fileNotices.selectedFile.currentRevision
        Button {
            objectName: "confirmResolution"
            text: qsTr("Confirm resolution")
            enabled: !fileNotices.controller.resolutionBusy && !fileNotices.composing
            onClicked: fileNotices.controller.markResolved(fileNotices.controller.selectedFileId, fileNotices.selectedFile.currentRevision, true)
        }
        Button {
            objectName: "cancelResolution"
            text: qsTr("Cancel")
            onClicked: fileNotices.controller.cancelResolution()
        }
    }
    Button {
        objectName: "retryRecovery"
        visible: !!fileNotices.selectedFile.pendingInput && fileNotices.selectedFile.recoveryState !== "save-failed"
        text: qsTr("Retry keeping pending input")
        onClicked: fileNotices.controller.files.retryPendingRecovery(fileNotices.controller.selectedFileId)
    }
    Button {
        objectName: "retrySave"
        visible: fileNotices.selectedFile.recoveryState === "save-failed"
        text: qsTr("Retry Save")
        onClicked: fileNotices.controller.files.retryPendingRecovery(fileNotices.controller.selectedFileId)
    }
    SnapshotPane {
        visible: !!fileNotices.selectedFile.pendingInput && fileNotices.selectedFile.recoveryState !== "save-failed"
        paneName: "pendingInput"
        paneTitle: qsTr("Pending input · not acknowledged")
        paneText: fileNotices.selectedFile.pendingText || ""
    }
}
