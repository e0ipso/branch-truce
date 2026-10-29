// One handoff entry: a category icon, the path as title, the status and each blocker on a
// line of its own, and the route to the file on the trailing side. A draft newer than the
// result staged in Git also offers to discard that draft.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: handoffEntry
    required property var modelData
    required property ApplicationController controller
    signal routed()
    // The file is selected; the owner confirms before the draft is discarded.
    signal discardStagedRequested()
    Layout.fillWidth: true
    spacing: Kirigami.Units.largeSpacing
    Kirigami.Icon {
        Layout.alignment: Qt.AlignTop
        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
        source: handoffEntry.modelData.icon
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        PlainLabel {
            Layout.fillWidth: true
            text: handoffEntry.modelData.path
            elide: Text.ElideMiddle
        }
        PlainLabel {
            Layout.fillWidth: true
            text: handoffEntry.modelData.status
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
        }
        Repeater {
            model: handoffEntry.modelData.blockers
            delegate: PlainLabel {
                required property string modelData
                Layout.fillWidth: true
                text: modelData
                wrapMode: Text.Wrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
            }
        }
    }
    ColumnLayout {
        Layout.alignment: Qt.AlignVCenter
        spacing: Kirigami.Units.smallSpacing
        Button {
            objectName: "handoffRoute-" + handoffEntry.modelData.fileId
            Layout.fillWidth: true
            icon.name: "go-jump"
            text: handoffEntry.modelData.actionLabel || qsTr("Open file")
            onClicked: {
                if (handoffEntry.controller.selectFile(handoffEntry.modelData.fileId))
                    handoffEntry.routed();
            }
        }
        Button {
            objectName: "handoffDiscardStaged-" + handoffEntry.modelData.fileId
            Layout.fillWidth: true
            visible: handoffEntry.modelData.action === "apply-or-discard"
            icon.name: "document-revert"
            text: qsTr("Discard draft and use staged result")
            onClicked: {
                if (handoffEntry.controller.selectFile(handoffEntry.modelData.fileId))
                    handoffEntry.discardStagedRequested();
            }
        }
    }
}
