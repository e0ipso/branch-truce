// One status fact as an icon and its text. A chip without an icon is the status sentence
// and has no background.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: chip
    required property var modelData
    // A long status sentence wraps inside the chip instead of overflowing the row.
    property real maximumWidth: Infinity
    readonly property bool framed: (modelData.icon || "").length > 0
    readonly property real padding: framed ? Kirigami.Units.largeSpacing : 0
    implicitWidth: chipRow.implicitWidth + padding
    implicitHeight: chipRow.implicitHeight + (framed ? Kirigami.Units.smallSpacing : 0)
    radius: Kirigami.Units.cornerRadius
    color: framed ? Kirigami.Theme.alternateBackgroundColor : "transparent"
    RowLayout {
        id: chipRow
        anchors.centerIn: parent
        spacing: Kirigami.Units.smallSpacing
        Kirigami.Icon {
            visible: chip.framed
            source: chip.modelData.icon || ""
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
        }
        PlainLabel {
            Layout.maximumWidth: chip.maximumWidth - chip.padding
                - (chip.framed ? Kirigami.Units.iconSizes.small + chipRow.spacing : 0)
            text: chip.modelData.text || ""
            wrapMode: Text.Wrap
        }
    }
}
