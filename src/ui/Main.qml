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
    function choose(target: string, action: string): void {
        if (!controller.previewChoice(controller.selectedFileId, selectedFile.currentRevision, target, action))
            return;
        if (controller.commandPreview.confirmationRequired)
            replacementDialog.open();
        else
            controller.applyChoice(false);
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: !!window.selectedFile.canUndo && !!window.selectedFile.canEdit && !resultEditor.inputMethodComposing
        onActivated: window.controller.undo(window.controller.selectedFileId, window.selectedFile.currentRevision)
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: !!window.selectedFile.canRedo && !!window.selectedFile.canEdit && !resultEditor.inputMethodComposing
        onActivated: window.controller.redo(window.controller.selectedFileId, window.selectedFile.currentRevision)
    }
    Dialog {
        id: replacementDialog
        objectName: "replacementDialog"
        title: qsTr("Replace manual work in this scope?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 700)
        height: Math.min(window.height - 40, 500)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: window.controller.applyChoice(true)
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.commandPreview.detail || ""
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                SnapshotPane {
                    paneName: "replacementBefore"
                    paneTitle: qsTr("Current text in this scope")
                    paneText: window.controller.commandPreview.before || ""
                }
                SnapshotPane {
                    paneName: "replacementAfter"
                    paneTitle: qsTr("Replacement from the selected source")
                    paneText: window.controller.commandPreview.replacement || ""
                }
            }
        }
    }
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
                    Label {
                        objectName: "commandMessage"
                        Layout.fillWidth: true
                        visible: window.controller.commandMessage.length > 0
                        text: window.controller.commandMessage
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
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
                        visible: window.supportedSelection
                        Button {
                            objectName: "undoCommand"
                            text: qsTr("Undo")
                            enabled: !!window.selectedFile.canEdit && !!window.selectedFile.canUndo && !resultEditor.inputMethodComposing
                            onClicked: window.controller.undo(window.controller.selectedFileId, window.selectedFile.currentRevision)
                        }
                        Button {
                            objectName: "redoCommand"
                            text: qsTr("Redo")
                            enabled: !!window.selectedFile.canEdit && !!window.selectedFile.canRedo && !resultEditor.inputMethodComposing
                            onClicked: window.controller.redo(window.controller.selectedFileId, window.selectedFile.currentRevision)
                        }
                        Label {
                            text: resultEditor.inputMethodComposing ? qsTr("Composing input · not acknowledged") : qsTr("Text and choices share undo/redo")
                        }
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
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Label {
                                objectName: "resultLabel"
                                text: qsTr("Editable result")
                            }
                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                TextArea {
                                    id: resultEditor
                                    objectName: "resultText"
                                    readOnly: true
                                    selectByMouse: true
                                    textFormat: TextEdit.PlainText
                                    font.family: "monospace"
                                    wrapMode: TextEdit.NoWrap
                                    Accessible.name: qsTr("Editable result")
                                    Component.onCompleted: window.controller.attachResultEditor(resultEditor)
                                }
                            }
                        }
                        SnapshotPane {
                            paneName: "stage3"
                            paneTitle: window.selectedFile.stage3
                                ? window.selectedFile.stage3.label + "\n" + window.selectedFile.stage3.objectId : ""
                            paneText: window.selectedFile.stage3 ? window.selectedFile.stage3.text : ""
                        }
                    }
                    ListView {
                        objectName: "scopedChoices"
                        visible: window.supportedSelection
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 180)
                        clip: true
                        model: window.selectedFile.choices || []
                        delegate: ColumnLayout {
                            id: choiceRow
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: choiceRow.modelData.label + " · " + choiceRow.modelData.state
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: choiceRow.modelData.sourceText || qsTr("Empty source scope · deletion")
                                textFormat: Text.PlainText
                                font.family: "monospace"
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }
                            RowLayout {
                                enabled: !!window.selectedFile.canEdit && !!choiceRow.modelData.available && !resultEditor.inputMethodComposing
                                Button {
                                    text: qsTr("Accept →")
                                    Accessible.name: qsTr("Accept %1").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "accept")
                                }
                                Button {
                                    text: qsTr("× Ignore")
                                    Accessible.name: qsTr("Ignore %1").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "ignore")
                                }
                                Button {
                                    text: qsTr("Resolve using this side")
                                    Accessible.name: qsTr("Resolve %1 using this side and ignore its competitor").arg(choiceRow.modelData.label)
                                    onClicked: window.choose(choiceRow.modelData.id, "resolve")
                                }
                            }
                            Label {
                                visible: !choiceRow.modelData.available
                                Layout.fillWidth: true
                                text: choiceRow.modelData.detail
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !!window.selectedFile.choiceNotice
                        text: window.selectedFile.choiceNotice || ""
                        wrapMode: Text.Wrap
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
                        paneText: (window.selectedFile.earlier && window.selectedFile.earlier.text) || ""
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
