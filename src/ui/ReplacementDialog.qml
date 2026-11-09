// The preview of a choice that replaces manual work or combines both sides, shown before it is applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

PreviewDialog {
    id: replacementDialog
    required property ChoicePreviewFlow choices
    required property CommandFeedback feedback
    required property SelectedFile selectedFile
    objectName: "replacementDialog"
    title: replacementDialog.choices.choicePreview.orderedCombination
        ? qsTr("Preview %1").arg(replacementDialog.choices.choicePreview.order)
        : qsTr("Replace manual work in this scope?")
    path: replacementDialog.selectedFile.path || ""
    summary: replacementDialog.choices.choicePreview.orderedCombination
        ? replacementDialog.choices.choicePreview.sourceScopes || "" : ""
    detail: replacementDialog.choices.choicePreview.detail || ""
    confirmText: qsTr("Apply choice")
    onApplyRequested: replacementDialog.refused = !replacementDialog.choices.applyChoice(true)
    refusalMessage: replacementDialog.feedback.message
    onRejected: replacementDialog.choices.cancel()
    PlainLabel {
        objectName: "manualReplacementNotice"
        Layout.fillWidth: true
        visible: !!replacementDialog.choices.choicePreview.replacesManual
        text: qsTr("Manual or pre-existing edits in the current scope below will be replaced. Confirm to replace this work, or cancel to keep it.")
        wrapMode: Text.Wrap
    }
    BeforeAfterPanes {
        namePrefix: "replacement"
        beforeTitle: qsTr("Current text in this scope")
        beforeText: replacementDialog.choices.choicePreview.before || ""
        afterTitle: replacementDialog.choices.choicePreview.orderedCombination
            ? qsTr("Proposed text · %1").arg(replacementDialog.choices.choicePreview.order)
            : qsTr("Replacement from the selected source")
        afterText: replacementDialog.choices.choicePreview.replacement || ""
    }
}
