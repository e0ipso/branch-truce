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
    // Controls at the header's far edge, after the title and identity. Set
    // showTrailing for a header that has them, so others keep their layout.
    property alias trailing: trailingControls.data
    property bool showTrailing: false
    // Font-scaled native units keep the wider side inset and shared text baseline.
    readonly property real sideInset: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    readonly property real titleIdentitySpacing: Kirigami.Units.largeSpacing
    readonly property real titleMinimumWidth: Math.min(Kirigami.Units.gridUnit * 2, header.width / 3)
    readonly property real titleNaturalWidth: Math.max(header.titleMinimumWidth,
        Math.min(paneTitle.implicitWidth, header.width / 3))
    readonly property real identityRowWidth: header.identity.length > 0
        ? identityLabel.implicitWidth + header.titleIdentitySpacing : 0
    // The width trailing controls can take while the title and identity keep
    // their natural widths. It ignores the controls' own width, so they can
    // pick a compact form from it without a binding loop.
    readonly property real trailingRoom: header.width - 2 * header.sideInset - header.titleNaturalWidth
        - header.identityRowWidth - header.titleIdentitySpacing
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
            Layout.minimumWidth: header.titleMinimumWidth
            // The complete identity outranks the title: with trailing controls in a
            // narrow header, the title gives up width and elides first.
            Layout.preferredWidth: header.showTrailing
                ? Math.max(header.titleMinimumWidth, Math.min(paneTitle.implicitWidth,
                    header.width - 2 * header.sideInset - header.identityRowWidth
                    - trailingControls.implicitWidth - header.titleIdentitySpacing))
                : paneTitle.implicitWidth
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
            PlainToolTip {
                id: identityToolTip
                objectName: header.identityName.length > 0 ? header.identityName + "ToolTip" : ""
                literalText: identityLabel.text + (header.description.length > 0 ? "\n" + header.description : "")
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
                // Layouts round allocated widths to pixels. Reserve the whole
                // text width so rounding cannot elide a fitting identity.
                Layout.preferredWidth: Math.ceil(implicitWidth)
                Layout.maximumWidth: Math.ceil(implicitWidth)
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
                Layout.preferredWidth: Math.ceil(implicitWidth)
                Layout.maximumWidth: Math.ceil(implicitWidth)
                font: Kirigami.Theme.fixedWidthFont
                elide: Text.ElideMiddle
                color: Kirigami.Theme.disabledTextColor
                Accessible.ignored: true
                ToolTip.visible: false
            }
        }
        // Spare width goes here and the controls start at the far edge, so they
        // never push the identity. They fill the text row's height, so the
        // header keeps the height of the headers beside it.
        RowLayout {
            id: trailingControls
            visible: header.showTrailing
            spacing: Kirigami.Units.smallSpacing
            layoutDirection: Qt.RightToLeft
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: implicitWidth
        }
    }
    Kirigami.Separator {
        id: separator
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
