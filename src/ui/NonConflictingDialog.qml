// The preview of applying the non-conflicting changes of a scope, shown before they are applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: nonConflictingDialog
    required property ApplicationController controller
    objectName: "nonConflictingDialog"
    title: qsTr("Apply non-conflicting changes · %1").arg(nonConflictingDialog.controller.nonConflictingPreview.scope || "")
    preferredWidth: Kirigami.Units.gridUnit * 42
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Apply | Kirigami.Dialog.Cancel
    onApplied: {
        nonConflictingDialog.controller.applyNonConflicting();
        close();
    }
    onRejected: nonConflictingDialog.controller.cancelChoice()
    PreviewBody {
        dialog: nonConflictingDialog
        Label {
            Layout.fillWidth: true
            text: nonConflictingDialog.controller.nonConflictingPreview.path || ""
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            text: nonConflictingDialog.controller.nonConflictingPreview.summary || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: nonConflictingDialog.controller.nonConflictingPreview.detail || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        ListView {
            objectName: "nonConflictingChanges"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: nonConflictingDialog.controller.nonConflictingPreview.changes || []
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
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 8
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
