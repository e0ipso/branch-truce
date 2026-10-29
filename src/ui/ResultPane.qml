// The editable result as a pane: its header above the text, with line numbers and
// provenance, diff colours, whitespace markers, a gutter column with each decided
// section's state, and the folded conflict-marker blocks as rows with their Take both menus.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: resultPane
    required property ApplicationController controller
    required property SelectedFile file
    required property EditActions edits
    // Line marks for the gutter and backgrounds, and the navigation target's line ranges.
    required property var marks
    required property var current
    // The folded conflict-marker blocks: hidden for the blocks, rows for the gutter and rows.
    required property var fold
    // The gutter's section decisions by line; see LineGutter.
    required property var decisions
    // The Take both offers of the folded rows by row line; see FoldedRows.
    required property var combinations
    // Whether the file takes choices now: it can be edited and no input is composing.
    required property bool choosing
    // The height every aligned pane gives its header, so the texts start on one row.
    required property real headerHeight
    required property real tabStop
    required property bool wrapLines
    required property bool showWhitespace
    // The text never gets shorter than this; on a short window the page scrolls instead.
    required property real minimumTextHeight
    property alias editor: resultEditor
    property alias viewport: resultViewport
    property alias blocks: resultBlocks
    readonly property real headerImplicitHeight: resultHeader.implicitHeight
    // byUser is set while the user scrolls the pane: dragging, flicking or the wheel, its
    // scroll bar, or keyboard focus in its text.
    signal lineHovered(int line)
    signal scrolled(bool byUser)
    // A Take both order for a section; the owner previews it.
    signal choiceChosen(target: string, action: int)
    signal choiceFocused(target: string, active: bool)
    // Moves the cursor off a folded line to where the blocks say it goes.
    function settleCursor(): void {
        const position = resultEditor.cursorPosition
        const anchor = resultEditor.selectionStart === position ? resultEditor.selectionEnd
            : resultEditor.selectionStart
        const target = resultBlocks.settledPosition(position, anchor)
        if (target === position)
            return
        if (anchor !== position)
            resultEditor.moveCursorSelection(target, TextEdit.SelectCharacters)
        else
            resultEditor.cursorPosition = target
    }
    Label {
        id: resultHeader
        objectName: "resultLabel"
        Layout.fillWidth: true
        Layout.preferredHeight: resultPane.headerHeight
        text: qsTr("Editable result · App draft")
        textFormat: Text.PlainText
        elide: Text.ElideRight
        HoverHandler { id: resultHeaderHover }
        ToolTip.visible: resultHeaderHover.hovered
        ToolTip.text: qsTr("Initial content and your edits are kept")
        ToolTip.delay: Kirigami.Units.toolTipDelay
        Accessible.description: ToolTip.text
    }
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: resultPane.minimumTextHeight
        Flickable {
            id: resultViewport
            objectName: "resultViewport"
            // TextArea.flickable moves the text's background into this Flickable, out of the
            // TextArea's View colour set; give the Flickable the same set as the gutter.
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
            anchors.fill: parent
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {}
            ScrollBar.vertical: ScrollBar {
                id: resultBar
            }
            HoverHandler {
                id: textHover
                function updateLine(): void {
                    const y = point.position.y + resultViewport.contentY - resultEditor.y - resultEditor.topPadding
                    resultPane.lineHovered(hovered ? Math.floor(resultBlocks.lineAtY(y)) : -1)
                }
                onHoveredChanged: updateLine()
                onPointChanged: if (hovered) updateLine()
            }
            onContentYChanged: resultPane.scrolled(moving || !!resultBar && resultBar.pressed
                || resultEditor.activeFocus)
            TextArea.flickable: TextArea {
                id: resultEditor
                objectName: "resultText"
                // The adapter permits edits only after a lossless, durable load.
                readOnly: !resultPane.controller.resultEditable
                selectByMouse: true
                persistentSelection: true
                textFormat: TextEdit.PlainText
                font: Kirigami.Theme.fixedWidthFont
                // Read by the C++ word-difference decoration: this pane, the line kinds its
                // backgrounds draw, and the text colours of those kinds' theme roles.
                readonly property var wordDifferenceStyle: ({
                    pane: "result",
                    lines: resultPane.marks.lines,
                    positive: Kirigami.Theme.positiveTextColor,
                    active: Kirigami.Theme.activeTextColor,
                    negative: Kirigami.Theme.negativeTextColor,
                    neutral: Kirigami.Theme.neutralTextColor,
                    disabled: Kirigami.Theme.disabledTextColor
                })
                tabStopDistance: resultPane.tabStop
                leftPadding: resultGutter.width + Kirigami.Units.smallSpacing
                width: resultPane.wrapLines ? resultViewport.width : implicitWidth
                wrapMode: resultPane.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                Accessible.name: qsTr("Editable result")
                Component.onCompleted: resultPane.controller.attachResultEditor(resultEditor)
                onCursorPositionChanged: resultBlocks.queueCursorCheck()
                // The editor still handles the key; the blocks note where it is taking the cursor.
                Keys.onPressed: (event) => {
                    resultBlocks.keyPressed(event.key)
                    event.accepted = false
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: resultMenu.popup()
                }
                ResultContextMenu {
                    id: resultMenu
                    controller: resultPane.controller
                    edits: resultPane.edits
                    editor: resultEditor
                }
            }
            FoldedRows {
                objectName: "resultFoldedRows"
                width: Math.max(resultEditor.width, resultViewport.width)
                height: resultGutter.height
                gutter: resultGutter
                combinations: resultPane.combinations
                choosing: resultPane.choosing
                current: resultPane.current
                onCombinationChosen: (target, action) => resultPane.choiceChosen(target, action)
                onChoiceFocused: (target, active) => resultPane.choiceFocused(target, active)
            }
            LineGutter {
                id: resultGutter
                objectName: "resultGutter"
                x: resultViewport.contentX
                width: implicitWidth
                height: Math.max(resultEditor.height, resultViewport.height)
                editor: resultEditor
                viewport: resultViewport
                provenance: true
                marks: resultPane.marks
                folds: resultPane.fold.rows
                decisionColumn: true
                decisions: resultPane.decisions
            }
            LineBackgrounds {
                objectName: "resultBackgrounds"
                width: Math.max(resultEditor.width, resultViewport.width)
                height: resultGutter.height
                gutter: resultGutter
                current: resultPane.current
            }
        }
        WhitespaceMarkers {
            objectName: "resultWhitespace"
            anchors.fill: parent
            anchors.leftMargin: resultGutter.width
            editor: resultEditor
            viewport: resultViewport
            blocks: resultBlocks
            active: resultPane.showWhitespace
        }
    }
    TextBlocks {
        id: resultBlocks
        document: resultEditor.textDocument
        hidden: resultPane.fold.hidden
        // A fold can hide the line the cursor is on.
        onLayoutChanged: resultBlocks.queueCursorCheck()
        onCursorCheckDue: resultPane.settleCursor()
    }
}
