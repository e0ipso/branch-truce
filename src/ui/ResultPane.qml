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
    spacing: 0
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
    required property real tabStop
    required property bool wrapLines
    required property bool showWhitespace
    // The text never gets shorter than this; on a short window the page scrolls instead.
    required property real minimumTextHeight
    property alias editor: resultEditor
    property alias viewport: resultViewport
    property alias blocks: resultBlocks
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
    PaneHeader {
        Layout.fillWidth: true
        titleName: "resultLabel"
        title: qsTr("Result")
        identity: qsTr("Editable")
        description: identity
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: resultPane.minimumTextHeight
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        color: Kirigami.Theme.backgroundColor
        Flickable {
            id: resultViewport
            objectName: "resultViewport"
            // TextArea.flickable moves the text's background into this Flickable, out of the
            // TextArea's View colour set; give the Flickable the same set as the gutter.
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
            anchors.fill: parent
            anchors.leftMargin: Kirigami.Units.smallSpacing
            anchors.rightMargin: Kirigami.Units.smallSpacing
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
                // Keep the style's own focus indication, but let the flat View
                // fill supply the idle background rather than an input frame.
                Binding {
                    target: resultEditor.background
                    property: "visible"
                    value: resultEditor.activeFocus
                }
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
                leftPadding: resultGutter.width + Kirigami.Units.largeSpacing
                rightPadding: Kirigami.Units.smallSpacing
                topPadding: Kirigami.Units.smallSpacing
                bottomPadding: Kirigami.Units.smallSpacing
                width: resultPane.wrapLines ? resultViewport.width : implicitWidth
                wrapMode: resultPane.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                Accessible.name: qsTr("Editable result")
                Component.onCompleted: resultPane.controller.attachResultEditor(resultEditor)
                // A document replacement must be spaced before the adapter restores
                // its cursor and viewport against the new layout.
                onTextChanged: resultBlocks.flushSpacing()
                onCursorPositionChanged: resultBlocks.queueCursorCheck()
                // Settle earlier cursor changes before recording this key's starting column.
                // The editor still handles the key; the blocks note where it takes the cursor.
                Keys.onPressed: (event) => {
                    resultPane.settleCursor()
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
                rowSpacing: Kirigami.Units.smallSpacing
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
            anchors.fill: resultViewport
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
        rowSpacing: Kirigami.Units.smallSpacing
        hidden: resultPane.fold.hidden
        // A fold can hide the line the cursor is on.
        onLayoutChanged: resultBlocks.queueCursorCheck()
        onCursorCheckDue: resultPane.settleCursor()
    }
}
