// One captured source as a read-only pane: its identity header above the text, with line
// numbers, diff colours and whitespace markers. A pane with a choiceEdge keeps a column of
// choiceWidth on that edge, beside the text, where ChoiceLane lays the region choices.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: sourcePane
    spacing: 0
    required property string paneName
    required property string paneRole
    required property var source
    property string versionName: ""
    // Line marks for the gutter and backgrounds, and the navigation target's line ranges.
    required property var marks
    required property var current
    required property real tabStop
    required property bool wrapLines
    required property bool showWhitespace
    // The text never gets shorter than this; on a short window the page scrolls instead.
    required property real minimumTextHeight
    property alias editor: sourceText
    property alias viewport: sourceViewport
    property alias blocks: sourceBlocks
    property EditActions edits: null
    // Optional source-range context menu supplied by the comparison's choice lane.
    property var openChoicesAt: null
    // The edge that faces the result, "left" or "right", for the choice column; none without it.
    property string choiceEdge: ""
    property real choiceWidth: 0
    readonly property alias choiceSlot: choiceColumn
    // Full identities remain available in the tooltips and accessibility descriptions.
    readonly property string commitId: sourcePane.source.commitId || ""
    readonly property string fullIdentity: qsTr("Verified commit: %1\nCaptured blob: %2")
        .arg(sourcePane.commitId || qsTr("unavailable"))
        .arg(sourcePane.source.objectId || qsTr("unavailable"))
    readonly property string displayIdentity: (sourcePane.source.shortCommitId || "").length > 0
        ? qsTr("%1: %2").arg(sourcePane.source.label || qsTr("Source identity unavailable"))
            .arg(sourcePane.source.shortCommitId)
        : (sourcePane.source.label || qsTr("Source identity unavailable"))
    // byUser is set while the user scrolls the pane: dragging, flicking or the wheel, its
    // scroll bar, or keyboard focus in its text.
    signal lineHovered(int line)
    signal scrolled(bool byUser)
    signal editorCompleted()
    PaneHeader {
        Layout.fillWidth: true
        titleName: sourcePane.paneName + "Label"
        title: sourcePane.versionName || sourcePane.source.label || sourcePane.paneRole
        identityName: sourcePane.paneName + "Identity"
        identity: sourcePane.versionName.length > 0
            ? qsTr("%1 · %2").arg(sourcePane.source.label || sourcePane.paneRole)
                .arg(sourcePane.source.shortCommitId || qsTr("Commit unavailable"))
            : sourcePane.source.shortCommitId || qsTr("Commit unavailable")
        identityRole: sourcePane.versionName.length > 0
            ? sourcePane.source.label || sourcePane.paneRole : ""
        identityHash: sourcePane.source.shortCommitId || ""
        description: sourcePane.fullIdentity
    }
    SourceContextMenuHost {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: sourcePane.minimumTextHeight
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        editor: sourceText
        onPointerMenuRequested: position => sourceText.openContextMenu(
            sourceBlocks.lineAt(sourceText.positionAt(position.x, position.y)), position.x, position.y)
        onKeyboardMenuRequested: {
            const rect = sourceText.cursorRectangle
            sourceText.openContextMenu(sourceBlocks.lineAt(sourceText.cursorPosition), rect.x, rect.y + rect.height)
        }
        Rectangle {
            anchors.fill: parent
            color: Kirigami.Theme.backgroundColor
        }
        Rectangle {
            id: choiceColumn
            objectName: sourcePane.paneName + "ChoiceColumn"
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            x: sourcePane.choiceEdge === "right" ? parent.width - width : 0
            width: sourcePane.choiceEdge.length > 0 ? sourcePane.choiceWidth : 0
            visible: width > 0
            color: Kirigami.Theme.backgroundColor
        }
        Flickable {
            id: sourceViewport
            objectName: sourcePane.paneName + "Viewport"
            // TextArea.flickable moves the text's background into this Flickable, out of the
            // TextArea's View colour set; give the Flickable the same set as the gutter.
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
            anchors.fill: parent
            anchors.leftMargin: (sourcePane.choiceEdge === "left" ? choiceColumn.width : 0) + Kirigami.Units.smallSpacing
            anchors.rightMargin: (sourcePane.choiceEdge === "right" ? choiceColumn.width : 0) + Kirigami.Units.smallSpacing
            // Only genuine unwrapped overflow reserves a native scrollbar row.
            anchors.bottomMargin: sourceHorizontal.visible ? sourceHorizontal.height : 0
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {
                id: sourceHorizontal
                objectName: sourcePane.paneName + "HorizontalScrollBar"
                parent: sourceViewport.parent
                anchors.left: sourceViewport.left
                anchors.right: sourceViewport.right
                anchors.bottom: parent.bottom
                height: implicitHeight
                policy: sourcePane.wrapLines ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
                Accessible.name: qsTr("%1 horizontal scroll").arg(sourcePane.paneRole)
            }
            ScrollBar.vertical: ScrollBar {
                id: sourceBar
            }
            HoverHandler {
                id: textHover
                function updateLine(): void {
                    const y = point.position.y + sourceViewport.contentY - sourceText.y - sourceText.topPadding
                    sourcePane.lineHovered(hovered ? Math.floor(sourceBlocks.lineAtY(y)) : -1)
                }
                onHoveredChanged: updateLine()
                onPointChanged: if (hovered) updateLine()
            }
            onContentYChanged: sourcePane.scrolled(moving || !!sourceBar && sourceBar.pressed
                || sourceText.activeFocus)
            TextArea.flickable: TextArea {
                id: sourceText
                objectName: sourcePane.paneName + "Text"
                text: sourcePane.source.text || ""
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                textFormat: TextEdit.PlainText
                font: Kirigami.Theme.fixedWidthFont
                // Keep the style's own focus indication, but let the flat View
                // fill supply the idle background rather than an input frame.
                Binding {
                    target: sourceText.background
                    property: "visible"
                    value: sourceText.activeFocus
                }
                // Read by the C++ word-difference decoration: this pane, the line kinds its
                // backgrounds draw, and the text colours of those kinds' theme roles.
                readonly property var wordDifferenceStyle: ({
                    pane: sourcePane.paneName,
                    lines: sourcePane.marks.lines,
                    positive: Kirigami.Theme.positiveTextColor,
                    active: Kirigami.Theme.activeTextColor,
                    negative: Kirigami.Theme.negativeTextColor,
                    neutral: Kirigami.Theme.neutralTextColor,
                    disabled: Kirigami.Theme.disabledTextColor
                })
                tabStopDistance: sourcePane.tabStop
                leftPadding: sourceGutter.width + Kirigami.Units.largeSpacing
                rightPadding: Kirigami.Units.smallSpacing
                topPadding: Kirigami.Units.smallSpacing
                bottomPadding: Kirigami.Units.smallSpacing
                // The native input background's minimum width is not text overflow.
                // TextArea.flickable also uses this implicit width for its content.
                implicitWidth: contentWidth + leftPadding + rightPadding
                width: sourcePane.wrapLines ? sourceViewport.width
                    : Math.max(sourceViewport.width, implicitWidth)
                wrapMode: sourcePane.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                Accessible.name: qsTr("%1 · %2").arg(sourcePane.paneRole).arg(sourcePane.displayIdentity)
                function openContextMenu(line: int, x: real, y: real): void {
                    sourceText.forceActiveFocus(Qt.PopupFocusReason)
                    if (sourcePane.openChoicesAt && sourcePane.openChoicesAt(line, x, y))
                        return
                    sourceMenu.popup(sourceText, x, y)
                }
                TapHandler {
                    id: touchContext
                    acceptedButtons: Qt.LeftButton
                    acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                    onLongPressed: sourceText.openContextMenu(
                        sourceBlocks.lineAt(sourceText.positionAt(touchContext.point.position.x, touchContext.point.position.y)),
                        touchContext.point.position.x, touchContext.point.position.y)
                }
                Menu {
                    id: sourceMenu
                    objectName: sourcePane.paneName + "ContextMenu"
                    onAboutToHide: sourceText.forceActiveFocus(Qt.PopupFocusReason)
                    MenuItem {
                        visible: !!sourcePane.edits
                        action: sourcePane.edits ? sourcePane.edits.undo : null
                    }
                    MenuItem {
                        visible: !!sourcePane.edits
                        action: sourcePane.edits ? sourcePane.edits.redo : null
                    }
                    MenuItem {
                        text: qsTr("Copy")
                        enabled: sourceText.selectedText.length > 0
                        onTriggered: sourceText.copy()
                    }
                    MenuItem {
                        text: qsTr("Select all")
                        onTriggered: sourceText.selectAll()
                    }
                }
                Component.onCompleted: sourcePane.editorCompleted()
            }
            LineGutter {
                id: sourceGutter
                objectName: sourcePane.paneName + "Gutter"
                x: sourceViewport.contentX
                width: implicitWidth
                height: Math.max(sourceText.height, sourceViewport.height)
                editor: sourceText
                blocks: sourceBlocks
                viewport: sourceViewport
                marks: sourcePane.marks
            }
            LineBackgrounds {
                objectName: sourcePane.paneName + "Backgrounds"
                width: Math.max(sourceText.width, sourceViewport.width)
                height: sourceGutter.height
                gutter: sourceGutter
                current: sourcePane.current
            }
        }
        WhitespaceMarkers {
            objectName: sourcePane.paneName + "Whitespace"
            anchors.fill: sourceViewport
            anchors.leftMargin: sourceGutter.width
            editor: sourceText
            viewport: sourceViewport
            blocks: sourceBlocks
            active: sourcePane.showWhitespace
        }
    }
    CodeRows {
        id: sourceBlocks
        document: sourceText.textDocument
    }
}
