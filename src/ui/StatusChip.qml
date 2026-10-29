// The status sentence stays readable; short status facts use the shared pill.
pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: chip
    required property var modelData
    property real maximumWidth: Infinity
    readonly property bool framed: (modelData.icon || "").length > 0
    implicitWidth: framed ? pill.implicitWidth : Math.min(maximumWidth, sentence.implicitWidth)
    implicitHeight: framed ? pill.implicitHeight : sentence.implicitHeight
    Pill {
        id: pill
        visible: chip.framed
        width: chip.width
        height: chip.height
        maximumWidth: chip.maximumWidth
        text: chip.modelData.text || ""
        iconName: chip.modelData.icon || ""
        tone: chip.modelData.tone || "neutral"
    }
    PlainLabel {
        id: sentence
        visible: !chip.framed
        width: chip.width
        text: chip.modelData.text || ""
        wrapMode: Text.Wrap
    }
}
