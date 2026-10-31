// The session's file list on a view background, kept scrolled to the selected file.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: fileListPane
    required property ApplicationController controller
    // Builds one row from the model's roles.
    required property Component rowDelegate
    property alias currentIndex: fileList.currentIndex
    property real headerHeight: Kirigami.Units.gridUnit * 2
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    color: Kirigami.Theme.backgroundColor
    RowLayout {
        id: listHeading
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Kirigami.Units.smallSpacing
        anchors.rightMargin: Kirigami.Units.smallSpacing
        height: Math.max(0, fileListPane.headerHeight - listSeparator.height)
        Kirigami.Heading { Layout.fillWidth: true; level: 5; type: Kirigami.Heading.Type.Primary; text: qsTr("Conflicted files") }
        Pill { text: String(fileListPane.controller.files.totalFiles) }
    }
    Kirigami.Separator { id: listSeparator; anchors.top: listHeading.bottom; anchors.left: parent.left; anchors.right: parent.right }
    ListView {
        id: fileList
        objectName: "fileList"
        Accessible.name: qsTr("File list")
        anchors.top: listSeparator.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        topMargin: Kirigami.Units.smallSpacing
        bottomMargin: Kirigami.Units.smallSpacing
        activeFocusOnTab: true
        keyNavigationEnabled: true
        clip: true
        model: fileListPane.controller.files
        currentIndex: 0
        function revealSelection(): void {
            const row = fileListPane.controller.selectedRow()
            if (row < 0 || row >= count)
                return
            currentIndex = row
            positionViewAtIndex(row, ListView.Contain)
        }
        Component.onCompleted: revealSelection()
        Connections {
            target: fileListPane.controller
            function onSelectedFileChanged(): void {
                fileList.revealSelection()
            }
        }
        delegate: fileListPane.rowDelegate
        ScrollBar.vertical: ScrollBar {}
    }
}
