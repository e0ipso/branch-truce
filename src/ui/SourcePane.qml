// One captured source as a read-only pane: its identity header above the text, with line
// numbers, diff colours and whitespace markers.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: sourcePane
    required property string paneName
    required property string paneRole
    required property var source
    // Line marks for the gutter and backgrounds, and the navigation target's line ranges.
    required property var marks
    required property var current
    // The height every aligned pane gives its header, so the texts start on one row.
    required property real headerHeight
    required property real tabStop
    required property bool wrapLines
    required property bool showWhitespace
    // The text never gets shorter than this; on a short window the page scrolls instead.
    required property real minimumTextHeight
    property alias editor: sourceText
    property alias viewport: sourceViewport
    property alias blocks: sourceBlocks
    property bool alignHeader: true
    readonly property real headerImplicitHeight: sourceHeaderRow.implicitHeight
    // The commit id is cut to 10 characters on the line; the tooltip keeps the full ids.
    readonly property string commitId: sourcePane.source.commitId || ""
    readonly property string fullIdentity: qsTr("Verified commit: %1\nCaptured blob: %2")
        .arg(sourcePane.commitId || qsTr("unavailable"))
        .arg(sourcePane.source.objectId || qsTr("unavailable"))
    // byUser is set while the user scrolls the pane: dragging, flicking or the wheel, its
    // scroll bar, or keyboard focus in its text.
    signal scrolled(bool byUser)
    signal textReplaced()
    signal editorCompleted()
    function shortCommit(label: string): string {
        return sourcePane.commitId.length > 10
            ? label.replace(sourcePane.commitId, sourcePane.commitId.slice(0, 10)) : label
    }
    RowLayout {
        id: sourceHeaderRow
        Layout.fillWidth: true
        Layout.preferredHeight: sourcePane.alignHeader ? sourcePane.headerHeight : implicitHeight
        Layout.rightMargin: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing
        Label {
            id: sourceHeader
            objectName: sourcePane.paneName + "Label"
            Layout.fillWidth: true
            text: sourcePane.paneRole + " · "
                + sourcePane.shortCommit(sourcePane.source.label || qsTr("Source identity unavailable"))
            textFormat: Text.PlainText
            elide: Text.ElideRight
            Accessible.description: sourcePane.fullIdentity
        }
        Label {
            objectName: sourcePane.paneName + "Identity"
            Layout.maximumWidth: sourceHeaderRow.width / 2
            text: qsTr("Captured blob: %1").arg(sourcePane.source.objectId
                ? sourcePane.source.objectId.slice(0, 10) : qsTr("unavailable"))
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Kirigami.Theme.disabledTextColor
            Accessible.description: sourcePane.fullIdentity
        }
        HoverHandler { id: identityHover }
        ToolTip.visible: identityHover.hovered
        ToolTip.text: sourcePane.fullIdentity
        ToolTip.delay: Kirigami.Units.toolTipDelay
    }
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: sourcePane.minimumTextHeight
        Flickable {
            id: sourceViewport
            objectName: sourcePane.paneName + "Viewport"
            anchors.fill: parent
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {}
            ScrollBar.vertical: ScrollBar {
                id: sourceBar
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
                leftPadding: sourceGutter.width + Kirigami.Units.smallSpacing
                width: sourcePane.wrapLines ? sourceViewport.width : implicitWidth
                wrapMode: sourcePane.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                Accessible.name: sourcePane.paneRole + " · " + (sourcePane.source.label || "")
                onTextChanged: sourcePane.textReplaced()
                Component.onCompleted: sourcePane.editorCompleted()
            }
            LineGutter {
                id: sourceGutter
                objectName: sourcePane.paneName + "Gutter"
                x: sourceViewport.contentX
                width: implicitWidth
                height: Math.max(sourceText.height, sourceViewport.height)
                editor: sourceText
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
            anchors.fill: parent
            anchors.leftMargin: sourceGutter.width
            editor: sourceText
            viewport: sourceViewport
            active: sourcePane.showWhitespace
            wrapped: sourcePane.wrapLines
        }
    }
    TextBlocks {
        id: sourceBlocks
        document: sourceText.textDocument
    }
}
