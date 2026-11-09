// The preview of replacing the whole result with one side, shown before it is applied.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

PreviewDialog {
    id: wholeFileDialog
    required property ChoicePreviewFlow choices
    required property CommandFeedback feedback
    objectName: "wholeFileDialog"
    title: qsTr("Replace whole file from %1").arg(wholeFileDialog.choices.wholeFilePreview.side || "")
    path: wholeFileDialog.choices.wholeFilePreview.path || ""
    summary: wholeFileDialog.choices.wholeFilePreview.source || ""
    detail: wholeFileDialog.choices.wholeFilePreview.detail || ""
    detailName: "wholeFileWarning"
    confirmText: qsTr("Replace whole file")
    onApplyRequested: wholeFileDialog.refused = !wholeFileDialog.choices.applyWholeFile()
    refusalMessage: wholeFileDialog.feedback.message
    onRejected: wholeFileDialog.choices.cancel()
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
                beforeText: wholeFileDialog.choices.wholeFilePreview.before || ""
                afterTitle: qsTr("Full proposed result")
                afterText: wholeFileDialog.choices.wholeFilePreview.replacement || ""
            }
            Item { Layout.fillHeight: true }
        }
        ColumnLayout {
            PlainLabel {
                Layout.fillWidth: true
                visible: !!wholeFileDialog.choices.wholeFilePreview.retainedDecisions
                text: wholeFileDialog.choices.wholeFilePreview.retainedDecisions || ""
                wrapMode: Text.Wrap
            }
            ListView {
                objectName: "wholeFileContributions"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: wholeFileDialog.choices.wholeFilePreview.contributions || []
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
                model: wholeFileDialog.choices.wholeFilePreview.differences || []
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
