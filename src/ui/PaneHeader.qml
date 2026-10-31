// One aligned header for a comparison pane, with a literal title and identity.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: header
    property string title: ""
    property string titleName: ""
    property string identity: ""
    property string identityName: ""
    property string description: ""
    property string identityRole: ""
    property string identityHash: ""
    // The historical native header used largeSpacing alone for the side inset.
    // Compose the reference's wider inset from font-scaled native units while
    // retaining the smaller title-to-identity gap and the shared text baseline.
    readonly property real sideInset: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    readonly property real titleIdentitySpacing: Kirigami.Units.largeSpacing
    implicitHeight: heading.implicitHeight + Kirigami.Units.mediumSpacing * 2 + separator.height
    color: Kirigami.Theme.alternateBackgroundColor
    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: separator.top
        anchors.leftMargin: header.sideInset
        anchors.rightMargin: header.sideInset
        spacing: header.titleIdentitySpacing
        // Heading supplies the native title font; PlainLabel renders the literal title.
        Kirigami.Heading {
            id: heading
            level: 5
            type: Kirigami.Heading.Type.Primary
            Layout.alignment: Qt.AlignBaseline
            Layout.minimumWidth: Math.min(Kirigami.Units.gridUnit * 2, header.width / 3)
            Layout.preferredWidth: paneTitle.implicitWidth
            Layout.maximumWidth: header.width / 3
            text: " "
            PlainLabel {
                id: paneTitle
                objectName: header.titleName
                anchors.fill: parent
                text: header.title
                font: heading.font
                Accessible.description: header.description
            }
        }
        RowLayout {
            id: identityLabel
            objectName: header.identityName
            visible: header.identity.length > 0
            // Keep one complete literal identity on the existing locator and
            // accessible node, even though its visible fonts are segmented.
            readonly property string text: header.identity
            readonly property bool truncated: roleText.truncated || hashText.truncated
            baselineOffset: Math.max(roleText.visible ? roleText.baselineOffset : 0,
                hashText.visible ? hashText.baselineOffset : 0)
            spacing: 0
            Layout.alignment: Qt.AlignBaseline
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: implicitWidth
            Layout.maximumWidth: implicitWidth
            Accessible.role: Accessible.StaticText
            Accessible.ignored: false
            Accessible.name: text
            Accessible.description: header.description
            HoverHandler { id: identityHover }
            ToolTip {
                text: identityLabel.text + (header.description.length > 0 ? "\n" + header.description : "")
                textFormat: Text.PlainText
                visible: identityHover.hovered && identityLabel.truncated
                delay: Kirigami.Units.toolTipDelay
            }
            PlainLabel {
                id: roleText
                objectName: header.identityName.length > 0 ? header.identityName + "Role" : ""
                text: header.identityHash.length > 0 ? header.identityRole : header.identity
                visible: text.length > 0
                Layout.alignment: Qt.AlignBaseline
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: implicitWidth
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                Accessible.ignored: true
                ToolTip.visible: false
            }
            PlainLabel {
                text: qsTr(" · ")
                visible: roleText.visible && hashText.visible
                Layout.alignment: Qt.AlignBaseline
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                Accessible.ignored: true
            }
            PlainLabel {
                id: hashText
                objectName: header.identityName.length > 0 ? header.identityName + "Hash" : ""
                text: header.identityHash
                visible: text.length > 0
                Layout.alignment: Qt.AlignBaseline
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: implicitWidth
                font: Kirigami.Theme.fixedWidthFont
                elide: Text.ElideMiddle
                color: Kirigami.Theme.disabledTextColor
                Accessible.ignored: true
                ToolTip.visible: false
            }
        }
    }
    Kirigami.Separator {
        id: separator
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
