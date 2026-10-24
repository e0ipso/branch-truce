pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window
    required property var controller
    readonly property ConflictRow selectedFile: fileList.currentItem as ConflictRow
    component ConflictRow: ItemDelegate {
        id: row
        required property int index
        required property string path
        required property string support
        required property string supportDetail
        required property string resultText
        width: ListView.view.width
        text: path + (support === "supported" ? ""
            : support === "unreadable" ? qsTr(" · unreadable") : qsTr(" · outside app"))
        highlighted: ListView.isCurrentItem
        contentItem: Label {
            text: row.text
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
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
                        onClicked: fileList.currentIndex = fileDelegate.index
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Label {
                        Layout.fillWidth: true
                        text: window.selectedFile ? window.selectedFile.path : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !!window.selectedFile && window.selectedFile.support !== "supported"
                        text: window.selectedFile ? window.selectedFile.supportDetail : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    Label {
                        visible: !!window.selectedFile && window.selectedFile.support === "supported"
                        text: qsTr("Current working file · preview")
                    }
                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: !!window.selectedFile && window.selectedFile.support === "supported"
                        TextArea {
                            text: window.selectedFile ? window.selectedFile.resultText : ""
                            readOnly: true
                            selectByMouse: true
                            textFormat: TextEdit.PlainText
                            font.family: "monospace"
                            wrapMode: TextEdit.NoWrap
                        }
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
