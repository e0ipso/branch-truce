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
    property Kirigami.Action helpfulAction: null
    // Asks the owner to set plain text on a message and each label inside it, once they exist.
    signal plainTextRequested(item: Item)
    // Extra items below the message, such as a busy indicator.
    default property alias content: message.data
    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.minimumHeight: message.implicitHeight
    PlainPlaceholderMessage {
        id: message
        objectName: placeholder.messageName
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 2
        onPlainTextRequested: item => placeholder.plainTextRequested(item)
        icon.name: placeholder.iconName
        text: placeholder.text
        explanation: placeholder.explanation
        helpfulAction: placeholder.helpfulAction
    }
}
