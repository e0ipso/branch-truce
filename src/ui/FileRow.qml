// A file list row: a status icon, file name and directory, and one status line.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
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
    required property int undecidedCount
    required property int conflictCount
    required property string recoveryState
    required property string status
    required property string summary
    required property ApplicationController controller
    readonly property bool current: fileId === row.controller.selectedFileId
    readonly property real plateOuterInset: Kirigami.Units.smallSpacing
    readonly property real plateHorizontalPadding: Kirigami.Units.largeSpacing
    readonly property real plateVerticalPadding: Kirigami.Units.mediumSpacing
    readonly property real iconTextGap: Kirigami.Units.largeSpacing
    // Inset the control itself so hover, clicks and focus share the plate's bounds.
    x: plateOuterInset
    width: Math.max(0, ListView.view.width - 2 * plateOuterInset)
    leftPadding: plateHorizontalPadding
    rightPadding: plateHorizontalPadding
    topPadding: plateVerticalPadding
    bottomPadding: plateVerticalPadding
    // Native styles may otherwise inset the background again or pad the last row differently.
    leftInset: 0
    rightInset: 0
    topInset: 0
    bottomInset: 0
    // The path alone names the row, so a screen reader reads it as the list item's name.
    text: row.path
    subtitle: row.recoveryState === "failed" || row.recoveryState === "save-failed" || row.recoveryState === "blocked"
        ? row.status
        : row.support === "unreadable" ? qsTr("Load error")
        : row.support !== "supported" ? qsTr("Handle outside the app")
        : row.status === "Written and resolved" ? qsTr("Resolved")
        : row.status === "Repository write failed" || row.status === "Written, unresolved"
            || row.status === "Draft save failed" || row.status === "Outcome uncertain; verification required"
            || row.status === "Earlier result staged; current draft not applied" ? row.status
        : row.conflictCount > 0 ? (row.conflictCount === 1
            ? qsTr("%n conflict left", "", row.conflictCount)
            : qsTr("%n conflicts left", "", row.conflictCount))
        : qsTr("No conflicts to step through")
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
        spacing: row.iconTextGap
        clip: true
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
            Item {
                id: pathLine
                Layout.fillWidth: true
                implicitHeight: Math.max(titleLabel.implicitHeight, directoryLabel.implicitHeight)
                PlainLabel {
                    id: titleLabel
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, pathLine.width)
                    text: row.path.split("/").pop() || row.path
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    elide: Text.ElideMiddle
                    font: Kirigami.Theme.defaultFont
                    color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                }
                PlainLabel {
                    id: directoryLabel
                    anchors.baseline: titleLabel.baseline
                    x: titleLabel.width + Kirigami.Units.smallSpacing
                    // The basename takes its natural width first; the directory uses only the remainder.
                    width: Math.min(implicitWidth, Math.max(0, pathLine.width - x))
                    visible: row.path.lastIndexOf("/") >= 0
                    text: row.path.slice(0, row.path.lastIndexOf("/"))
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    elide: Text.ElideMiddle
                    font: Kirigami.Theme.smallFont
                    color: row.highlighted ? Qt.alpha(Kirigami.Theme.highlightedTextColor, 0.8) : Kirigami.Theme.disabledTextColor
                }
            }
            PlainLabel {
                id: subtitleLabel
                Layout.fillWidth: true
                text: row.subtitle
                wrapMode: Text.NoWrap
                maximumLineCount: 1
                elide: Text.ElideRight
                font: Kirigami.Theme.smallFont
                color: row.highlighted ? Qt.alpha(Kirigami.Theme.highlightedTextColor, 0.8) : row.support !== "supported" ? Kirigami.Theme.neutralTextColor : row.status === "Written and resolved" ? Kirigami.Theme.positiveTextColor : Kirigami.Theme.disabledTextColor
            }
        }

    }
    background: Rectangle {
        radius: Kirigami.Units.cornerRadius
        color: row.current ? Kirigami.Theme.highlightColor
            : row.hovered ? Qt.alpha(Kirigami.Theme.hoverColor, 0.2) : "transparent"
        border.width: row.activeFocus ? 1 : 0
        border.color: Kirigami.Theme.focusColor
    }
}
