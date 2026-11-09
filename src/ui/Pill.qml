// A short count tag, such as the number of conflicted files beside the list heading.
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami

Rectangle {
    id: pill
    property alias text: caption.text
    implicitWidth: caption.implicitWidth + Kirigami.Units.smallSpacing * 2
    implicitHeight: caption.implicitHeight + Kirigami.Units.smallSpacing
    radius: height / 2
    color: Qt.alpha(Kirigami.Theme.disabledTextColor, 0.14)
    border.width: 1
    border.color: Qt.alpha(Kirigami.Theme.disabledTextColor, 0.4)
    PlainLabel {
        id: caption
        anchors.centerIn: parent
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
    }
}
