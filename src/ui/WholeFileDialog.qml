// The preview of replacing the whole result with one side, shown before it is applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

PreviewDialog {
    id: wholeFileDialog
    required property ApplicationController controller
    objectName: "wholeFileDialog"
    title: qsTr("Replace whole file from %1").arg(wholeFileDialog.controller.wholeFilePreview.side || "")
    path: wholeFileDialog.controller.wholeFilePreview.path || ""
    summary: wholeFileDialog.controller.wholeFilePreview.source || ""
    detail: wholeFileDialog.controller.wholeFilePreview.detail || ""
    detailName: "wholeFileWarning"
    confirmText: qsTr("Replace whole file")
    onConfirmed: wholeFileDialog.controller.applyWholeFile()
    onRejected: wholeFileDialog.controller.cancelChoice()
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
        ColumnLayout {
            BeforeAfterPanes {
                namePrefix: "wholeFile"
                beforeTitle: qsTr("Full current result, including manual edits")
                beforeText: wholeFileDialog.controller.wholeFilePreview.before || ""
                afterTitle: qsTr("Full proposed result")
                afterText: wholeFileDialog.controller.wholeFilePreview.replacement || ""
            }
            Item { Layout.fillHeight: true }
        }
        ColumnLayout {
            PlainLabel {
                Layout.fillWidth: true
                visible: !!wholeFileDialog.controller.wholeFilePreview.retainedDecisions
                text: wholeFileDialog.controller.wholeFilePreview.retainedDecisions || ""
                wrapMode: Text.Wrap
            }
            ListView {
                objectName: "wholeFileContributions"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: wholeFileDialog.controller.wholeFilePreview.contributions || []
                spacing: Kirigami.Units.largeSpacing
                delegate: ColumnLayout {
                    id: wholeContribution
                    required property var modelData
                    width: ListView.view.width
                    PlainLabel {
                        Layout.fillWidth: true
                        text: qsTr("%1\n%2 → %3").arg(wholeContribution.modelData.scope)
                            .arg(wholeContribution.modelData.beforeState).arg(wholeContribution.modelData.afterState)
                        wrapMode: Text.Wrap
                    }
                    SnapshotPane {
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 7
                        paneName: "wholeContributionSource"
                        paneTitle: qsTr("Source contribution")
                        paneText: wholeContribution.modelData.sourceText
                    }
                }
                ScrollBar.vertical: ScrollBar {}
            }
        }
        ColumnLayout {
            PlainLabel {
                Layout.fillWidth: true
                text: qsTr("All differences from the selected source are replaced, including manual and pre-existing edits. Identical text stays the same.")
                wrapMode: Text.Wrap
            }
            ListView {
                objectName: "wholeFileDifferences"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: wholeFileDialog.controller.wholeFilePreview.differences || []
                spacing: Kirigami.Units.largeSpacing
                delegate: ScopedChange {
                    id: wholeDifference
                    required property var modelData
                    width: ListView.view.width
                    scope: modelData.scope
                    namePrefix: "wholeDifference"
                    beforeTitle: qsTr("Current text being replaced")
                    beforeText: modelData.before
                    afterTitle: qsTr("Replacement text")
                    afterText: modelData.replacement
                }
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }
}
