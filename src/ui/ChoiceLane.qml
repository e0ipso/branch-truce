// Source choices overlay the full viewport without reserving text width. Each range's
// controls sit on its first line at the edge facing the result and appear on hover or focus.
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: lane
    required property SourcePane pane
    // The pane's choice rows: {line, choice, extras}, from ComparisonView.choiceRowsFor.
    required property var rows
    required property real rowHeight
    // Whether the file takes choices now: it can be edited and no input is composing.
    required property bool choosing
    signal chosen(target: string, action: int)
    signal hovered(target: string)
    signal focused(target: string, active: bool)
    // Context inspection uses the source editor, including decided ranges and
    // precise lines after a range's first row. The popup never anchors to chrome
    // that disappears when a choice completes.
    function openMenuForLine(line: int, x: real, y: real): bool {
        if (!visible)
            return false
        for (let i = 0; i < rows.length; ++i) {
            if (line >= rows[i].line && line < Math.max(rows[i].line + 1, rows[i].end)) {
                const row = choiceRows.itemAt(i) as ChoiceButtons
                return !!row && row.openMenuAt(pane.editor, x, y)
            }
        }
        return false
    }
    // Counts relayouts of the pane's text, so the rows follow them.
    property int layoutRevision: 0
    // Explicit geometry arguments keep the mapped viewport current across split/header changes.
    function viewportIn(paneX: real, paneY: real, paneWidth: real, paneHeight: real,
                        viewportX: real, viewportY: real, viewportWidth: real, viewportHeight: real, surfaceY: real): rect {
        const origin = pane.viewport.mapToItem(lane.parent, 0, 0)
        return Qt.rect(origin.x, origin.y, viewportWidth, viewportHeight)
    }
    // The y in the lane of a 1-based line's top, clamped to the pane's lines. revision is
    // layoutRevision, so a binding follows relayouts.
    function lineTop(line: int, revision: int, contentY: real): real {
        const shown = Math.min(Math.max(line, 1), Math.max(pane.blocks.blockCount, 1))
        return pane.editor.y + pane.editor.topPadding + pane.blocks.yAtLine(shown) - contentY
    }
    readonly property rect viewportRect: viewportIn(pane.x, pane.y, pane.width, pane.height,
        pane.viewport.x, pane.viewport.y, pane.viewport.width, pane.viewport.height, pane.viewport.parent.y)
    x: viewportRect.x
    y: viewportRect.y
    width: viewportRect.width
    height: viewportRect.height
    visible: width > 0
    clip: true
    // A stationary pointer must follow wrapping and font relayouts too.
    function lineAtPointer(y: real, revision: int, contentY: real): int {
        return Math.floor(pane.blocks.lineAtY(y + contentY - pane.editor.y - pane.editor.topPadding))
    }
    readonly property int hoveredLine: pointer.hovered
        ? lineAtPointer(pointer.point.position.y, layoutRevision, pane.viewport.contentY) : -1
    HoverHandler {
        id: pointer
        blocking: false
    }
    Connections {
        target: lane.pane.blocks
        function onLayoutChanged(): void { ++lane.layoutRevision }
    }
    Repeater {
        id: choiceRows
        model: lane.rows
        delegate: ChoiceButtons {
            id: row
            readonly property real rowTop: lane.lineTop(row.modelData.line, lane.layoutRevision, lane.pane.viewport.contentY)
            edge: lane.pane.choiceEdge
            rowHeight: lane.rowHeight
            rowHovered: lane.hoveredLine >= row.modelData.line
                && lane.hoveredLine < Math.max(row.modelData.line + 1, row.modelData.end)
            choosing: lane.choosing
            contextEditor: lane.pane.editor
            edits: lane.pane.edits
            x: edge === "right" ? lane.width - width - Kirigami.Units.smallSpacing : Kirigami.Units.smallSpacing
            y: rowTop
            visible: rowTop >= 0 && rowTop + lane.rowHeight <= lane.height
            onHovered: target => lane.hovered(target)
            onFocused: (target, active) => lane.focused(target, active)
            onChosen: (target, action) => lane.chosen(target, action)
        }
    }
}
