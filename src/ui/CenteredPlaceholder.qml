// A plain-text placeholder message centred in the space its layout gives it.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The layout never makes it shorter than the message, so the page scrolls instead of cutting
// the message off. Its objectName goes on the message, which tests read like a placeholder.
Item {
    id: placeholder
    required property string messageName
    property string iconName
    property string text
    property string explanation
    property bool viewBackground: false
    property Kirigami.Action helpfulAction: null
    // Keep prose near the approved reading measure while following native font sizing.
    readonly property real maximumReadingWidth: Kirigami.Units.gridUnit * 26
    readonly property real contentMargin: Kirigami.Units.gridUnit
    // Asks the owner to set plain text on a message and each label inside it, once they exist.
    signal plainTextRequested(item: Item)
    // Extra items below the message, such as a busy indicator.
    default property alias content: message.data
    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.minimumHeight: message.implicitHeight + contentMargin * 2
    Rectangle {
        parent: placeholder
        anchors.fill: parent
        z: -1
        visible: placeholder.viewBackground
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        color: Kirigami.Theme.backgroundColor
    }
    PlainPlaceholderMessage {
        id: message
        objectName: placeholder.messageName
        anchors.centerIn: parent
        width: Math.min(placeholder.maximumReadingWidth,
            Math.max(0, placeholder.width - placeholder.contentMargin * 2))
        onPlainTextRequested: item => placeholder.plainTextRequested(item)
        icon.name: placeholder.iconName
        text: placeholder.text
        explanation: placeholder.explanation
        helpfulAction: placeholder.helpfulAction
    }
}
