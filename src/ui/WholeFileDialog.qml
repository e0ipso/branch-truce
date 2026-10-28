// The preview of replacing the whole result with one side, shown before it is applied.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: wholeFileDialog
    required property var controller
    objectName: "wholeFileDialog"
    title: qsTr("Replace whole file from %1").arg(wholeFileDialog.controller.wholeFilePreview.side || "")
    preferredWidth: Kirigami.Units.gridUnit * 52
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Apply | Kirigami.Dialog.Cancel
    onApplied: {
        wholeFileDialog.controller.applyWholeFile();
        close();
    }
    onRejected: wholeFileDialog.controller.cancelChoice()
    PreviewBody {
        dialog: wholeFileDialog
        Label {
            Layout.fillWidth: true
            text: wholeFileDialog.controller.wholeFilePreview.path || ""
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            text: wholeFileDialog.controller.wholeFilePreview.source || ""
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        Label {
            objectName: "wholeFileWarning"
            Layout.fillWidth: true
            text: wholeFileDialog.controller.wholeFilePreview.detail || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
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
            RowLayout {
                SnapshotPane {
                    paneName: "wholeFileBefore"
                    paneTitle: qsTr("Full current result, including manual edits")
                    paneText: wholeFileDialog.controller.wholeFilePreview.before || ""
                }
                SnapshotPane {
                    paneName: "wholeFileAfter"
                    paneTitle: qsTr("Full proposed result")
                    paneText: wholeFileDialog.controller.wholeFilePreview.replacement || ""
                }
            }
            ColumnLayout {
                Label {
                    Layout.fillWidth: true
                    visible: !!wholeFileDialog.controller.wholeFilePreview.retainedDecisions
                    text: wholeFileDialog.controller.wholeFilePreview.retainedDecisions || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                ListView {
                    objectName: "wholeFileContributions"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: wholeFileDialog.controller.wholeFilePreview.contributions || []
                    delegate: ColumnLayout {
                        id: wholeContribution
                        required property var modelData
                        width: ListView.view.width
                        Label {
                            Layout.fillWidth: true
                            text: wholeContribution.modelData.scope + "\n"
                                + wholeContribution.modelData.beforeState + " → " + wholeContribution.modelData.afterState
                            textFormat: Text.PlainText
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
                Label {
                    Layout.fillWidth: true
                    text: qsTr("All differences from the selected source are replaced, including manual and pre-existing edits. Identical text stays the same.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                ListView {
                    objectName: "wholeFileDifferences"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: wholeFileDialog.controller.wholeFilePreview.differences || []
                    delegate: ColumnLayout {
                        id: wholeDifference
                        required property var modelData
                        width: ListView.view.width
                        Label {
                            Layout.fillWidth: true
                            text: wholeDifference.modelData.scope
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                            SnapshotPane {
                                paneName: "wholeDifferenceBefore"
                                paneTitle: qsTr("Current text being replaced")
                                paneText: wholeDifference.modelData.before
                            }
                            SnapshotPane {
                                paneName: "wholeDifferenceAfter"
                                paneTitle: qsTr("Replacement text")
                                paneText: wholeDifference.modelData.replacement
                            }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }
    }
}
