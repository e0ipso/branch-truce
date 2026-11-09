// The preview of applying the non-conflicting changes of a scope, shown before they are applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

PreviewDialog {
    id: nonConflictingDialog
    required property ChoicePreviewFlow choices
    required property CommandFeedback feedback
    objectName: "nonConflictingDialog"
    title: qsTr("Apply non-conflicting changes · %1").arg(nonConflictingDialog.choices.nonConflictingPreview.scope || "")
    path: nonConflictingDialog.choices.nonConflictingPreview.path || ""
    summary: nonConflictingDialog.choices.nonConflictingPreview.summary || ""
    detail: nonConflictingDialog.choices.nonConflictingPreview.detail || ""
    confirmText: qsTr("Apply changes")
    onApplyRequested: nonConflictingDialog.refused = !nonConflictingDialog.choices.applyNonConflicting()
    refusalMessage: nonConflictingDialog.feedback.message
    onRejected: nonConflictingDialog.choices.cancel()
    ListView {
        objectName: "nonConflictingChanges"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: nonConflictingDialog.choices.nonConflictingPreview.changes || []
        spacing: Kirigami.Units.largeSpacing
        delegate: ScopedChange {
            required property var modelData
            width: ListView.view.width
            scope: modelData.scope
            namePrefix: "batch"
            beforeTitle: qsTr("Current text in this scope")
            beforeText: modelData.before
            afterTitle: qsTr("Proposed text")
            afterText: modelData.replacement
        }
        ScrollBar.vertical: ScrollBar {}
    }
}
