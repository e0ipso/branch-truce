// The preview of applying the non-conflicting changes of a scope, shown before they are applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

PreviewDialog {
    id: nonConflictingDialog
    required property ApplicationController controller
    objectName: "nonConflictingDialog"
    title: qsTr("Apply non-conflicting changes · %1").arg(nonConflictingDialog.controller.nonConflictingPreview.scope || "")
    path: nonConflictingDialog.controller.nonConflictingPreview.path || ""
    summary: nonConflictingDialog.controller.nonConflictingPreview.summary || ""
    detail: nonConflictingDialog.controller.nonConflictingPreview.detail || ""
    confirmText: qsTr("Apply changes")
    onConfirmed: nonConflictingDialog.controller.applyNonConflicting()
    onRejected: nonConflictingDialog.controller.cancelChoice()
    ListView {
        objectName: "nonConflictingChanges"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: nonConflictingDialog.controller.nonConflictingPreview.changes || []
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
