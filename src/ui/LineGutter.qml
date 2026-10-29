// Line numbers for one pane, one per document block, so a wrapped line keeps
// a single number. The gutter sits in the pane's Flickable beside the
// TextArea: its y coordinates are the editor's and it scrolls vertically with
// the text. Where it is used, x follows contentX to keep it at the left edge,
// width is implicitWidth and the editor pads its text by that width.
//
// A column between the numbers and the text shows the
// glyph of each line in marks.lines, and the gaps in marks.gaps on its
// right edge. rows lists the blocks near the view
// as {number, position, y, height, top, extent}, where top and extent span
// the whole block. With provenance set, a column before the slot shows the
// letter in marks.letters. folds maps the row of each folded
// conflict-marker block, its opening line, to the block's {last} line; the
// row keeps its number and glyph, and the layers over the text read folds
// to draw it.
//
// With decisionColumn set, a column after the slot holds the result's
// section decisions, keyed by line. decisions maps a line to the {state,
// label} of the section that starts there once both its sides are decided,
// and the column shows that state as a chip.
//
// rowSlots holds one entry per row. A Repeater over it, with each delegate
// reading rows[index], keeps its delegates when a refresh changes rows but
// not their number, as typing on a line does. A delegate the Repeater has
// removed but not yet destroyed reads noRow, which shows nothing.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Item {
    id: gutter
    required property TextArea editor
    required property Flickable viewport
    property var rows: []
    property var marks: ({lines: {}, gaps: {}, letters: {}})
    property var folds: ({})
    property bool provenance: false
    property bool decisionColumn: false
    property var decisions: ({})
    readonly property int digits: Math.max(2, String(blocks.blockCount).length)
    readonly property real numberWidth: Math.ceil(digits * gutterFont.averageCharacterWidth)
    readonly property real letterWidth: provenance
        ? Math.ceil(gutterFont.averageCharacterWidth) + Kirigami.Units.smallSpacing : 0
    // The editor range, with one view height either side, that rows covers.
    property real coveredTop: 0
    property real coveredBottom: -1
    z: 1
    implicitWidth: Kirigami.Units.smallSpacing * 2 + numberWidth + letterWidth + slot.width + decisionSlot.width
        + separator.width
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    Accessible.ignored: true

    property bool refreshQueued: false
    function schedule(): void {
        if (refreshQueued)
            return
        refreshQueued = true
        Qt.callLater(gutter.refresh)
    }
    function invalidate(): void {
        coveredBottom = -1
        schedule()
    }
    function follow(): void {
        const top = viewport.contentY - editor.y
        if (top < coveredTop || top + viewport.height > coveredBottom)
            schedule()
    }
    function refresh(): void {
        refreshQueued = false
        const span = viewport.height
        const top = Math.max(0, viewport.contentY - editor.y - span)
        const bottom = viewport.contentY - editor.y + 2 * span
        const found = blocks.between(top - editor.topPadding, bottom - editor.topPadding)
        const next = []
        let same = found.length === rows.length
        for (let i = 0; i < found.length; ++i) {
            const rect = editor.positionToRectangle(found[i].position)
            const row = {number: found[i].number, position: found[i].position,
                y: editor.y + rect.y, height: rect.height,
                top: editor.y + editor.topPadding + found[i].top, extent: found[i].height}
            same = same && rows[i].number === row.number && rows[i].position === row.position
                && rows[i].y === row.y && rows[i].height === row.height
                && rows[i].top === row.top && rows[i].extent === row.extent
            next.push(row)
        }
        coveredTop = top
        coveredBottom = bottom
        if (same)
            return
        // Every slot keeps a row: drop the extra slots first and add the new ones last.
        if (rowSlots.count > next.length)
            rowSlots.remove(next.length, rowSlots.count - next.length)
        rows = next
        while (rowSlots.count < next.length)
            rowSlots.append({})
    }
    readonly property ListModel rowSlots: ListModel {}
    readonly property var noRow: ({number: 0, position: 0, y: 0, height: 0, top: 0, extent: 0})
    Component.onCompleted: schedule()

    TextBlocks {
        id: blocks
        document: gutter.editor.textDocument
        onLayoutChanged: gutter.invalidate()
    }
    FontMetrics {
        id: gutterFont
        font: gutter.editor.font
    }
    Connections {
        target: gutter.editor
        function onWidthChanged(): void { gutter.invalidate() }
        function onFontChanged(): void { gutter.invalidate() }
        function onTopPaddingChanged(): void { gutter.invalidate() }
    }
    Connections {
        target: gutter.viewport
        function onContentYChanged(): void { gutter.follow() }
        function onHeightChanged(): void { gutter.invalidate() }
    }
    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.backgroundColor
    }
    // Carry the folded row's background through its line number and ! glyph.
    Repeater {
        model: gutter.rowSlots
        delegate: Rectangle {
            required property int index
            readonly property var modelData: gutter.rows[index] || gutter.noRow
            visible: !!gutter.folds[modelData.number]
            y: modelData.top
            width: gutter.width
            height: modelData.extent
            color: Kirigami.Theme.negativeBackgroundColor
        }
    }
    Repeater {
        model: gutter.rowSlots
        delegate: PlainLabel {
            required property int index
            readonly property var modelData: gutter.rows[index] || gutter.noRow
            x: Kirigami.Units.smallSpacing
            y: modelData.y
            width: gutter.numberWidth
            height: modelData.height
            text: String(modelData.number)
            font: gutter.editor.font
            color: Kirigami.Theme.disabledTextColor
            horizontalAlignment: Text.AlignRight
        }
    }
    Repeater {
        model: gutter.provenance ? gutter.rowSlots : 0
        delegate: PlainLabel {
            required property int index
            readonly property var modelData: gutter.rows[index] || gutter.noRow
            x: Kirigami.Units.smallSpacing * 2 + gutter.numberWidth
            y: modelData.y
            width: gutter.letterWidth
            height: modelData.height
            text: gutter.marks.letters[modelData.number] || ""
            visible: text.length > 0
            font: gutter.editor.font
            color: Kirigami.Theme.disabledTextColor
        }
    }
    Item {
        id: slot
        objectName: gutter.objectName + "MarkerSlot"
        x: Kirigami.Units.smallSpacing * 2 + gutter.numberWidth + gutter.letterWidth
        width: Kirigami.Units.gridUnit
        height: gutter.height
        Repeater {
            model: gutter.rowSlots
            delegate: PlainLabel {
                id: kindGlyph
                required property int index
                readonly property var modelData: gutter.rows[index] || gutter.noRow
                readonly property int line: modelData.number
                readonly property string kind: gutter.marks.lines[line] || ""
                y: modelData.y
                height: modelData.height
                text: kind.length > 0 ? DiffStyle.glyph(kind) : ""
                visible: text.length > 0
                font: gutter.editor.font
                color: DiffStyle.textColor(kind, kindGlyph)
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                Accessible.description: kind.length > 0 ? DiffStyle.description(kind) : ""
            }
        }
        Repeater {
            model: gutter.rowSlots
            delegate: PlainLabel {
                id: gapGlyph
                required property int index
                readonly property var modelData: gutter.rows[index] || gutter.noRow
                readonly property int line: modelData.number
                readonly property string kind: gutter.marks.gaps[line] ? "removed" : ""
                // Repeater detaches delegates before destroying them.
                anchors.right: parent ? parent.right : undefined
                y: modelData.top - height / 2
                text: kind.length > 0 ? DiffStyle.glyph(kind) : ""
                visible: text.length > 0
                font: gutter.editor.font
                color: DiffStyle.textColor(kind, gapGlyph)
                Accessible.description: kind.length > 0 ? DiffStyle.description(kind) : ""
            }
        }
    }
    Item {
        id: decisionSlot
        objectName: gutter.objectName + "DecisionSlot"
        x: slot.x + slot.width
        width: gutter.decisionColumn ? Math.ceil(gutterFont.lineSpacing) + Kirigami.Units.smallSpacing : 0
        height: gutter.height
        visible: width > 0
        Repeater {
            model: gutter.decisionColumn ? gutter.rowSlots : 0
            delegate: Item {
                id: decisionRow
                required property int index
                readonly property var modelData: gutter.rows[index] || gutter.noRow
                readonly property int line: modelData.number
                readonly property var decision: gutter.decisions[line] || null
                y: modelData.y
                width: decisionSlot.width
                height: modelData.height
                // The decided section's state as a short tag in its role's colour.
                Pill {
                    id: decisionChip
                    readonly property string decision: decisionRow.decision ? decisionRow.decision.state : ""
                    text: decision.length > 0 ? DiffStyle.glyph(decision) : ""
                    tone: DiffStyle.pillTone(decision)
                    visible: text.length > 0
                    anchors.centerIn: parent
                    height: decisionRow.height
                    width: Math.max(height, implicitWidth)
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                    Accessible.description: decisionRow.decision ? decisionRow.decision.label : ""
                }
            }
        }
    }
    Kirigami.Separator {
        id: separator
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
    }
}
