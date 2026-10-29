// A short, themed state tag shared by the file list and status controls.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: pill
    property alias text: caption.text
    property string iconName: ""
    property string tone: "neutral"
    property real maximumWidth: Infinity
    readonly property color toneColor: tone === "positive" ? Kirigami.Theme.positiveTextColor
        : tone === "negative" ? Kirigami.Theme.negativeTextColor
        : tone === "warning" ? Kirigami.Theme.neutralTextColor
        : tone === "highlight" ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
    implicitWidth: Math.min(maximumWidth, contents.implicitWidth + Kirigami.Units.smallSpacing * 2)
    implicitHeight: contents.implicitHeight + Kirigami.Units.smallSpacing
    radius: height / 2
    color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.14)
    border.width: 1
    border.color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.4)
    RowLayout {
        id: contents
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.smallSpacing
        anchors.rightMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing
        Kirigami.Icon {
            visible: pill.iconName.length > 0
            source: pill.iconName
            Layout.preferredWidth: visible ? Kirigami.Units.iconSizes.small : 0
            Layout.preferredHeight: visible ? Kirigami.Units.iconSizes.small : 0
        }
        PlainLabel {
            id: caption
            Layout.fillWidth: true
            font: Kirigami.Theme.smallFont
            color: pill.toneColor
        }
    }
}
