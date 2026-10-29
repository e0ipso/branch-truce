// The preview of a choice that replaces manual work or combines both sides, shown before it is applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: replacementDialog
    required property ApplicationController controller
    objectName: "replacementDialog"
    title: replacementDialog.controller.commandPreview.orderedCombination
        ? qsTr("Preview %1").arg(replacementDialog.controller.commandPreview.order)
        : qsTr("Replace manual work in this scope?")
    preferredWidth: Kirigami.Units.gridUnit * 40
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Ok | Kirigami.Dialog.Cancel
    onAccepted: replacementDialog.controller.applyChoice(true)
    onRejected: replacementDialog.controller.cancelChoice()
    PreviewBody {
        dialog: replacementDialog
        PlainLabel {
            Layout.fillWidth: true
            text: replacementDialog.controller.commandPreview.detail || ""
            wrapMode: Text.Wrap
        }
        PlainLabel {
            Layout.fillWidth: true
            visible: !!replacementDialog.controller.commandPreview.orderedCombination
            text: replacementDialog.controller.commandPreview.sourceScopes || ""
            wrapMode: Text.Wrap
        }
        PlainLabel {
            objectName: "manualReplacementNotice"
            Layout.fillWidth: true
            visible: !!replacementDialog.controller.commandPreview.replacesManual
            text: qsTr("Manual or pre-existing edits in the current scope below will be replaced. Confirm to replace this work, or cancel to keep it.")
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            SnapshotPane {
                paneName: "replacementBefore"
                paneTitle: qsTr("Current text in this scope")
                paneText: replacementDialog.controller.commandPreview.before || ""
            }
            SnapshotPane {
                paneName: "replacementAfter"
                paneTitle: replacementDialog.controller.commandPreview.orderedCombination
                    ? qsTr("Proposed text · %1").arg(replacementDialog.controller.commandPreview.order)
                    : qsTr("Replacement from the selected source")
                paneText: replacementDialog.controller.commandPreview.replacement || ""
            }
        }
    }
}
