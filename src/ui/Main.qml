pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window
    required property var controller
    readonly property var selectedFile: controller.selectedFile
    readonly property bool supportedSelection: selectedFile.support === "supported"
    component ConflictRow: ItemDelegate {
        id: row
        required property int index
        required property string fileId
        required property string support
        required property string supportDetail
        required property string status
        required property bool unsaved
        required property bool reviewNeeded
        required property bool resolved
        width: ListView.view.width
        text: window.controller.displayPath(fileId) + (support === "supported" ? ""
            : support === "unreadable" ? qsTr(" · load error · outside app") : qsTr(" · outside app"))
        highlighted: fileId === window.controller.selectedFileId
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true
                text: row.text
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                visible: row.support !== "supported"
                text: row.supportDetail
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: row.status + "\n" + (row.unsaved ? qsTr("Unsaved draft") : qsTr("Manual Save checkpoint retained"))
                    + (row.reviewNeeded ? qsTr(" · needs review") : "")
                    + (row.resolved ? "" : qsTr(" · unresolved"))
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
        }
    }
    component SnapshotPane: ColumnLayout {
        id: pane
        required property string paneName
        required property string paneTitle
        required property string paneText
        Layout.fillWidth: true
        Layout.fillHeight: true
        Label {
            objectName: pane.paneName + "Label"
            Layout.fillWidth: true
            text: pane.paneTitle
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                objectName: pane.paneName + "Text"
                text: pane.paneText
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                font.family: "monospace"
                wrapMode: TextEdit.NoWrap
                Accessible.name: pane.paneTitle
            }
        }
    }
    width: 1000
    height: 700
    visible: true
    title: qsTr("Branch Truce")
    pageStack.initialPage: Kirigami.Page {
        title: window.controller.operation || qsTr("Open conflict session")
        ColumnLayout {
            anchors.fill: parent
            spacing: Kirigami.Units.largeSpacing
            Label {
                Layout.fillWidth: true
                text: window.controller.repositoryRoot
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            RowLayout {
                BusyIndicator { running: window.controller.opening; visible: running }
                Label {
                    Layout.fillWidth: true
                    text: window.controller.detail
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 files in this session · %2 resolved")
                    .arg(window.controller.files.totalFiles).arg(window.controller.files.resolvedFiles)
                visible: !window.controller.opening && window.controller.files.totalFiles > 0
            }
            Kirigami.InlineMessage {
                objectName: "inventoryWarning"
                Layout.fillWidth: true
                visible: !window.controller.opening
                    && window.controller.outsideAppFiles + window.controller.loadErrorFiles > 0
                type: Kirigami.MessageType.Warning
                text: qsTr("%1 outside app · %2 load errors. These files remain outstanding. Select a file for its reason and handle it outside Branch Truce.")
                    .arg(window.controller.outsideAppFiles).arg(window.controller.loadErrorFiles)
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: !window.controller.opening && window.controller.files.totalFiles > 0
                ListView {
                    id: fileList
                    Layout.preferredWidth: 260
                    Layout.fillHeight: true
                    clip: true
                    model: window.controller.files
                    currentIndex: 0
                    delegate: ConflictRow {
                        id: fileDelegate
                        onClicked: {
                            fileList.currentIndex = fileDelegate.index;
                            window.controller.selectFile(fileDelegate.fileId);
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Label {
                        Layout.fillWidth: true
                        objectName: "selectedPath"
                        text: window.selectedFile.path || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        objectName: "selectedProblem"
                        visible: window.controller.selectedFileId.length > 0 && !window.supportedSelection
                        text: (window.selectedFile.support === "unreadable"
                            ? qsTr("Load error · outside app · outstanding\n") : qsTr("Outside app · outstanding\n"))
                            + (window.selectedFile.supportDetail || "")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Label {
                        objectName: "selectedStatus"
                        Layout.fillWidth: true
                        visible: window.controller.selectedFileId.length > 0
                        text: (window.selectedFile.status || "") + "\n"
                            + (window.selectedFile.unsaved ? qsTr("Unsaved draft") : qsTr("Manual Save checkpoint retained"))
                            + (window.selectedFile.reviewNeeded ? qsTr(" · needs review") : "")
                            + (window.selectedFile.resolved ? "" : qsTr(" · unresolved"))
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Kirigami.InlineMessage {
                        objectName: "recoveryProblem"
                        Layout.fillWidth: true
                        visible: !!window.selectedFile.recoveryError
                        type: Kirigami.MessageType.Error
                        text: window.selectedFile.recoveryError || ""
                    }
                    Button {
                        objectName: "retryRecovery"
                        visible: !!window.selectedFile.pendingInput
                        text: qsTr("Retry keeping pending input")
                        onClicked: window.controller.files.retryPendingRecovery(window.controller.selectedFileId)
                    }
                    SnapshotPane {
                        visible: !!window.selectedFile.pendingInput
                        paneName: "pendingInput"
                        paneTitle: qsTr("Pending input · not acknowledged")
                        paneText: window.selectedFile.pendingText || ""
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: window.supportedSelection
                        SnapshotPane {
                            paneName: "stage2"
                            paneTitle: window.selectedFile.stage2
                                ? window.selectedFile.stage2.label + "\n" + window.selectedFile.stage2.objectId : ""
                            paneText: window.selectedFile.stage2 ? window.selectedFile.stage2.text : ""
                        }
                        SnapshotPane {
                            paneName: "result"
                            paneTitle: qsTr("Result · preview")
                            paneText: window.selectedFile.resultText || ""
                        }
                        SnapshotPane {
                            paneName: "stage3"
                            paneTitle: window.selectedFile.stage3
                                ? window.selectedFile.stage3.label + "\n" + window.selectedFile.stage3.objectId : ""
                            paneText: window.selectedFile.stage3 ? window.selectedFile.stage3.text : ""
                        }
                    }
                    CheckBox {
                        id: showEarlier
                        objectName: "showEarlier"
                        visible: window.supportedSelection
                        enabled: !!window.selectedFile.earlier && !!window.selectedFile.earlier.available
                        text: enabled ? qsTr("Show common earlier version") : qsTr("Common earlier version unavailable")
                    }
                    SnapshotPane {
                        visible: window.supportedSelection && showEarlier.enabled && showEarlier.checked
                        paneName: "earlier"
                        paneTitle: window.selectedFile.earlier
                            ? window.selectedFile.earlier.label + "\n" + window.selectedFile.earlier.objectId : ""
                        paneText: window.selectedFile.earlier ? window.selectedFile.earlier.text : ""
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: window.controller.stage2.length > 0
                text: window.controller.stage2 + "\n" + window.controller.stage3
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.workflow
                wrapMode: Text.Wrap
            }
        }
    }
}
