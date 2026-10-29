// One aligned header for a comparison pane, with a literal title and identity.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: header
    property string title: ""
    property string titleName: ""
    property string identity: ""
    property string identityName: ""
    property string description: ""
    implicitHeight: heading.implicitHeight + Kirigami.Units.mediumSpacing * 2 + separator.height
    color: Kirigami.Theme.alternateBackgroundColor
    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: separator.top
        anchors.leftMargin: Kirigami.Units.largeSpacing
        anchors.rightMargin: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing
        // Heading supplies the native title font; PlainLabel renders the literal title.
        Kirigami.Heading {
            id: heading
            level: 5
            Layout.fillWidth: true
            text: " "
            PlainLabel {
                objectName: header.titleName
                anchors.fill: parent
                text: header.title
                font: heading.font
                Accessible.description: header.description
            }
        }
        PlainLabel {
            objectName: header.identityName
            visible: header.identity.length > 0
            Layout.fillWidth: true
            Layout.preferredWidth: header.width / 2
            Layout.maximumWidth: header.width / 2
            text: header.identity
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            Accessible.description: header.description
        }
    }
    Kirigami.Separator {
        id: separator
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
