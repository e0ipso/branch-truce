// The session's file list on a view background, kept scrolled to the selected file.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Rectangle {
    id: fileListPane
    required property var controller
    // Builds one row from the model's roles, with the icon and status lines computed for it.
    required property Component rowDelegate
    property alias currentIndex: fileList.currentIndex
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    color: Kirigami.Theme.backgroundColor
    ListView {
        id: fileList
        objectName: "fileList"
        Accessible.name: qsTr("File list")
        anchors.fill: parent
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
