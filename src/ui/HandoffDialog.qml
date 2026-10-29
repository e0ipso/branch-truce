// The session handoff: what is finished, what stays outstanding, and how to finish or leave.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: handoffDialog
    required property ApplicationController controller
    // Asks the owner to set plain text on a message and each label inside it, once they exist.
    signal plainTextRequested(item: Item)
    // Builds one entry of a group from its handoff entry.
    required property Component rowDelegate
    readonly property var summary: controller.handoff
    objectName: "handoffDialog"
    title: qsTr("Session handoff")
    preferredWidth: Kirigami.Units.gridUnit * 36
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Close
    footerLeadingComponent: RowLayout {
        Button {
            objectName: "handoffFinish"
            visible: !!handoffDialog.summary.clean
            icon.name: "dialog-ok-apply"
            text: qsTr("Finish")
            onClicked: handoffDialog.controller.finishSession()
        }
        Button {
            objectName: "handoffLeave"
            visible: !handoffDialog.summary.clean
            enabled: !!handoffDialog.summary.canLeave
            icon.name: "application-exit"
            text: qsTr("Leave with work outstanding")
            onClicked: handoffDialog.controller.leaveSession()
        }
    }
    DialogBody {
        PlainInlineMessage {
            objectName: "handoffOutcome"
            Layout.fillWidth: true
            onPlainTextRequested: item => handoffDialog.plainTextRequested(item)
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
                spacing: Kirigami.Units.smallSpacing
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 4
                    text: qsTr("%1 (%2)").arg(handoffGroup.modelData.title).arg(handoffGroup.modelData.entries.length)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: handoffGroup.modelData.entries
                    delegate: handoffDialog.rowDelegate
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
    }
}
