// A file list row: the path, the status line and a chip for a file outside the app.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.delegates as KirigamiDelegates

KirigamiDelegates.SubtitleDelegate {
    id: row
    required property int index
    required property string fileId
    required property string support
    required property string path
    required property string statusIcon
    required property string statusLine
    required property string summary
    required property var controller
    readonly property bool current: fileId === row.controller.selectedFileId
    readonly property string chip: support === "unreadable" ? qsTr("Load error")
        : support === "supported" ? "" : qsTr("Outside app")
    width: ListView.view.width
    // The path alone names the row, so a screen reader reads it as the list item's name.
    text: row.path
    subtitle: row.statusLine
    Accessible.description: row.summary
    icon.name: row.statusIcon
    highlighted: current
    // The style's shared tooltip fades out after the pointer leaves its row. Opened below a row
    // near the top of the list, it covers the next rows and takes their clicks while it fades,
    // so each row shows its own tooltip, which closes as soon as the row loses the pointer.
    ToolTip.visible: false
    // It holds the summary, which the row does not show, and a status line the row elides.
    ToolTip {
        text: row.text + "\n\n" + (subtitleLabel.truncated ? row.subtitle + "\n\n" : "") + row.summary
        visible: row.hovered
        delay: Kirigami.Units.toolTipDelay
        exit: null
        // A path or a status line is plain text, as the shared tooltip shows it.
        Component.onCompleted: {
            const pending = [contentItem];
            while (pending.length > 0) {
                const next = pending.pop();
                if (next["textFormat"] !== undefined)
                    next["textFormat"] = Text.PlainText;
                pending.push(...next.children);
            }
        }
    }
    Keys.onReturnPressed: row.clicked()
    Keys.onEnterPressed: row.clicked()
    // One line each, so a path or a status never breaks inside a word.
    contentItem: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        Kirigami.Icon {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            source: row.icon.name
            selected: row.highlighted
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Label {
                id: titleLabel
                Layout.fillWidth: true
                text: row.text
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                font.bold: row.current
                color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
            }
            Label {
                id: subtitleLabel
                Layout.fillWidth: true
                text: row.subtitle
                textFormat: Text.PlainText
                maximumLineCount: 1
                elide: Text.ElideRight
                font: Kirigami.Theme.smallFont
                color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
            }
        }
        Label {
            Layout.alignment: Qt.AlignVCenter
            visible: row.chip.length > 0
            text: row.chip
            textFormat: Text.PlainText
            font: Kirigami.Theme.smallFont
            leftPadding: Kirigami.Units.smallSpacing
            rightPadding: Kirigami.Units.smallSpacing
            color: Kirigami.Theme.neutralTextColor
            background: Rectangle {
                color: Kirigami.Theme.neutralBackgroundColor
                radius: Kirigami.Units.cornerRadius
            }
        }
    }
    Kirigami.Separator {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: row.index < row.ListView.view.count - 1
    }
}
