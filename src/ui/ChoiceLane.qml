// One source pane's choices, laid over the column the pane keeps for them on its edge that
// faces the result. Each row sits on the first line of its scope, at that line's y in the
// pane less the pane's scroll, as the connector bands do. The lane clips to the column and
// hides a row that is not wholly inside it.
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
    // Counts relayouts of the pane's text, so the rows follow them.
    property int layoutRevision: 0
    // The pane's choice column in this lane's parent. The arguments are the geometry the
    // column moves with, so the binding follows the split, the pane and its header.
    function columnIn(paneX: real, paneWidth: real, paneHeight: real, textTop: real): rect {
        const slot = pane.choiceSlot
        const origin = slot.mapToItem(lane.parent, 0, 0)
        return Qt.rect(origin.x, origin.y, slot.width, slot.height)
    }
    // The y in the lane of a 1-based line's top, clamped to the pane's lines. revision is
    // layoutRevision, so a binding follows relayouts.
    function lineTop(line: int, revision: int, contentY: real): real {
        const shown = Math.min(Math.max(line, 1), Math.max(pane.blocks.blockCount, 1))
        return pane.editor.y + pane.editor.topPadding + pane.blocks.yAtLine(shown) - contentY
    }
    readonly property rect column: columnIn(pane.x, pane.width, pane.height, pane.choiceSlot.parent.y)
    x: column.x
    y: column.y
    width: column.width
    height: column.height
    visible: width > 0
    clip: true
    Connections {
        target: lane.pane.blocks
        function onLayoutChanged(): void { ++lane.layoutRevision }
    }
    Repeater {
        model: lane.rows
        delegate: ChoiceButtons {
            id: row
            readonly property real top: lane.lineTop(row.modelData.line, lane.layoutRevision, lane.pane.viewport.contentY)
            edge: lane.pane.choiceEdge
            rowHeight: lane.rowHeight
            choosing: lane.choosing
            x: edge === "right" ? lane.width - width - Kirigami.Units.smallSpacing : Kirigami.Units.smallSpacing
            y: top
            visible: top >= 0 && top + lane.rowHeight <= lane.height
            onChosen: (target, action) => lane.chosen(target, action)
        }
    }
}
