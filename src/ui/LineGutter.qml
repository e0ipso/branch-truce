// Line numbers for one pane, one per document block, so a wrapped line keeps
// a single number. The gutter sits in the pane's Flickable beside the
// TextArea: its y coordinates are the editor's and it scrolls vertically with
// the text. Where it is used, x follows contentX to keep it at the left edge,
// width is implicitWidth and the editor pads its text by that width.
//
// markerSlot is a column between the numbers and the text. It shows the
// glyph of each line in marks.lines, and the gaps in marks.gaps on its
// right edge; per-region actions go there too. Place an item in it at the
// y of editor.positionToRectangle(position); rows lists the blocks near
// the view as {number, position, y, height, top, extent}, where top and
// extent span the whole block. With provenance set, a column before the
// slot shows the letter in marks.letters. folds maps the first line below
// each run of folded lines to that run; on a run that opens a conflict-
// marker block the slot shows a chip saying that markers remain, in place
// of the line's glyph.
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
    readonly property alias markerSlot: slot
    readonly property int digits: Math.max(2, String(blocks.blockCount).length)
    readonly property real numberWidth: Math.ceil(digits * gutterFont.averageCharacterWidth)
    readonly property real letterWidth: provenance
        ? Math.ceil(gutterFont.averageCharacterWidth) + Kirigami.Units.smallSpacing : 0
    // The editor range, with one view height either side, that rows covers.
    property real coveredTop: 0
    property real coveredBottom: -1
    z: 1
    implicitWidth: Kirigami.Units.smallSpacing * 2 + numberWidth + letterWidth + slot.width + separator.width
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
    Repeater {
        model: gutter.rowSlots
        delegate: Text {
            required property int index
            readonly property var modelData: gutter.rows[index] || gutter.noRow
            x: Kirigami.Units.smallSpacing
            y: modelData.y
            width: gutter.numberWidth
            height: modelData.height
            text: String(modelData.number)
            textFormat: Text.PlainText
            font: gutter.editor.font
            color: Kirigami.Theme.disabledTextColor
            horizontalAlignment: Text.AlignRight
        }
    }
    Repeater {
        model: gutter.provenance ? gutter.rowSlots : 0
        delegate: Text {
            required property int index
            readonly property var modelData: gutter.rows[index] || gutter.noRow
            x: Kirigami.Units.smallSpacing * 2 + gutter.numberWidth
            y: modelData.y
            width: gutter.letterWidth
            height: modelData.height
            text: gutter.marks.letters[modelData.number] || ""
            visible: text.length > 0
            textFormat: Text.PlainText
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
            delegate: Text {
                required property int index
                readonly property var modelData: gutter.rows[index] || gutter.noRow
                readonly property int line: modelData.number
                readonly property string kind: gutter.marks.lines[line] || ""
                y: modelData.y
                height: modelData.height
                text: ({added: "+", changed: "~", conflict: "!", manual: "m",
                    accepted: "✓", ignored: "×", markers: "!"})[kind] || ""
                visible: text.length > 0 && !(gutter.folds[line] && gutter.folds[line].chip)
                textFormat: Text.PlainText
                font: gutter.editor.font
                color: kind === "added" || kind === "accepted" ? Kirigami.Theme.positiveTextColor
                    : kind === "changed" ? Kirigami.Theme.activeTextColor
                    : kind === "conflict" || kind === "markers" ? Kirigami.Theme.negativeTextColor
                    : kind === "ignored" ? Kirigami.Theme.disabledTextColor
                    : Kirigami.Theme.neutralTextColor
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                Accessible.description: ({added: qsTr("Added line"), changed: qsTr("Changed line"),
                    conflict: qsTr("Undecided conflict"), manual: qsTr("Hand-edited line"),
                    accepted: qsTr("Accepted side"), ignored: qsTr("Ignored side"),
                    markers: qsTr("Conflict markers remain")})[kind] || ""
            }
        }
        Repeater {
            model: gutter.rowSlots
            delegate: Rectangle {
                id: chip
                required property int index
                readonly property var modelData: gutter.rows[index] || gutter.noRow
                readonly property int line: modelData.number
                readonly property var fold: gutter.folds[line] || null
                readonly property string part: "chip"
                readonly property string kind: fold && fold.chip ? "markers" : ""
                readonly property string text: kind.length > 0 ? "!" : ""
                visible: kind.length > 0
                y: modelData.y
                width: Math.max(chipText.implicitWidth + Kirigami.Units.smallSpacing, height / 2)
                height: modelData.height
                radius: Kirigami.Units.cornerRadius
                color: Kirigami.Theme.negativeBackgroundColor
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                Accessible.description: !fold ? ""
                    : fold.count === 1 ? qsTr("Conflict markers remain: 1 marker line hidden above")
                    : qsTr("Conflict markers remain: %1 marker lines hidden above").arg(fold.count)
                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: chip.text
                    textFormat: Text.PlainText
                    font: gutter.editor.font
                    color: Kirigami.Theme.negativeTextColor
                }
            }
        }
        Repeater {
            model: gutter.rowSlots
            delegate: Text {
                required property int index
                readonly property var modelData: gutter.rows[index] || gutter.noRow
                readonly property int line: modelData.number
                readonly property string kind: gutter.marks.gaps[line] ? "removed" : ""
                anchors.right: parent.right
                y: modelData.top - height / 2
                text: kind.length > 0 ? "\u2212" : ""
                visible: text.length > 0
                textFormat: Text.PlainText
                font: gutter.editor.font
                color: Kirigami.Theme.disabledTextColor
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
