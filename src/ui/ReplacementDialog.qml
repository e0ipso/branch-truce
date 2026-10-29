// The preview of a choice that replaces manual work or combines both sides, shown before it is applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

PreviewDialog {
    id: replacementDialog
    required property ApplicationController controller
    objectName: "replacementDialog"
    title: replacementDialog.controller.commandPreview.orderedCombination
        ? qsTr("Preview %1").arg(replacementDialog.controller.commandPreview.order)
        : qsTr("Replace manual work in this scope?")
    path: replacementDialog.controller.selectedFile.path || ""
    summary: replacementDialog.controller.commandPreview.orderedCombination
        ? replacementDialog.controller.commandPreview.sourceScopes || "" : ""
    detail: replacementDialog.controller.commandPreview.detail || ""
    confirmText: qsTr("Apply choice")
    onConfirmed: replacementDialog.controller.applyChoice(true)
    onRejected: replacementDialog.controller.cancelChoice()
    PlainLabel {
        objectName: "manualReplacementNotice"
        Layout.fillWidth: true
        visible: !!replacementDialog.controller.commandPreview.replacesManual
        text: qsTr("Manual or pre-existing edits in the current scope below will be replaced. Confirm to replace this work, or cancel to keep it.")
        wrapMode: Text.Wrap
    }
    BeforeAfterPanes {
        namePrefix: "replacement"
        beforeTitle: qsTr("Current text in this scope")
        beforeText: replacementDialog.controller.commandPreview.before || ""
        afterTitle: replacementDialog.controller.commandPreview.orderedCombination
            ? qsTr("Proposed text · %1").arg(replacementDialog.controller.commandPreview.order)
            : qsTr("Replacement from the selected source")
        afterText: replacementDialog.controller.commandPreview.replacement || ""
    }
}
