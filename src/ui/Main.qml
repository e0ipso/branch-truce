import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    width: 1000
    height: 700
    visible: true
    title: qsTr("Branch Truce")
    pageStack.initialPage: Kirigami.Page {
        title: qsTr("Branch Truce")
        Label {
            anchors.centerIn: parent
            text: qsTr("Branch Truce")
        }
    }
}
