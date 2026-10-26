pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: comparison
    required property var controller
    required property var file
    readonly property bool inputMethodComposing: resultEditor.inputMethodComposing
    readonly property real headerHeight: Math.max(leftPane.headerImplicitHeight,
        resultHeader.implicitHeight, rightPane.headerImplicitHeight)
    readonly property bool wrapLines: wrapLinesBox.checked
    readonly property bool showWhitespace: showWhitespaceBox.checked
    property var scopeNames: ["stage2", "result", "stage3"]
    property string searchDetail: ""
    // Text panes never get shorter than this many lines; on a short window the page scrolls instead.
    property int minimumPaneLines: 8
    property bool whitespaceReady: false
    // The controller picks Breeze Light or Breeze Dark syntax colours from its lightness.
    readonly property color schemeBackground: Kirigami.Theme.backgroundColor
    readonly property string searchStatusText: searchDetail.length > 0
        ? searchDetail
        : qsTr("Search scope: %1.").arg(searchScope.currentText || qsTr("Editable result"))

    function editorNamed(name): Item {
        if (name === "stage2")
            return leftPane.editor
        if (name === "stage3")
            return rightPane.editor
        if (name === "earlier")
            return earlierPane.editor
        return resultEditor
    }
    function refreshSearchScopes(): void {
        const labels = [qsTr("Left source"), qsTr("Editable result"), qsTr("Right source")]
        const names = ["stage2", "result", "stage3"]
        if (showEarlier.enabled && showEarlier.checked) {
            labels.push(qsTr("Earlier version"))
            names.push("earlier")
        }
        const current = scopeNames[searchScope.currentIndex] || "result"
        scopeNames = names
        searchScope.model = labels
        const next = names.indexOf(current)
        searchScope.currentIndex = next >= 0 ? next : names.indexOf("result")
    }
    function applyWhitespace(): void {
        const visible = showWhitespaceBox.checked
        const editors = [leftPane.editor, resultEditor, rightPane.editor, earlierPane.editor]
        for (let i = 0; i < editors.length; ++i)
            controller.setVisibleWhitespace(editors[i], visible)
    }
    function findPassage(): void {
        const index = searchScope.currentIndex
        const name = index >= 0 && index < scopeNames.length ? scopeNames[index] : "result"
        const editor = editorNamed(name)
        const scope = searchScope.currentText || qsTr("Editable result")
        const query = searchQuery.text
        if (!editor || query.length === 0) {
            searchDetail = qsTr("Search scope: %1. Enter a passage.").arg(scope)
            return
        }
        const haystack = editor.text
        let start = 0
        if (editor.selectedText === query)
            start = editor.selectionEnd
        let found = haystack.indexOf(query, start)
        if (found < 0 && start > 0)
            found = haystack.indexOf(query, 0)
        if (found < 0) {
            searchDetail = qsTr("No passage in %1.").arg(scope)
            return
        }
        editor.select(found, found + query.length)
        followScroll(name, "")
        searchDetail = qsTr("Passage in %1.").arg(scope)
    }
    // Line marks for one pane, keyed by 1-based line. lines holds the kind of
    // each coloured line: "added" when the compared text lacks it, "changed",
    // "conflict" inside an undecided region, or "manual" inside a hand-edited
    // result region. gaps holds the lines above which the compared text has
    // lines this pane lacks. letters holds the result's provenance: L, R or B
    // for a line equal to the left, right or both sources, m for a hand edit.
    function marksFor(pane: string, file: var): var {
        const lines = {}
        const gaps = {}
        const letters = {}
        const rank = {added: 1, changed: 2, manual: 3, conflict: 4}
        const mark = function(first, end, kind) {
            for (let line = first; line < end; ++line) {
                if (!lines[line] || rank[lines[line]] < rank[kind])
                    lines[line] = kind
            }
        }
        const result = pane === "result"
        const from = {}
        const passages = file.passages || []
        for (let i = 0; i < passages.length; ++i) {
            const passage = passages[i]
            if (result ? passage.side === "earlier" : passage.side !== pane)
                continue
            const first = result ? passage.resultLineBegin : passage.sourceLineBegin
            const end = result ? passage.resultLineEnd : passage.sourceLineEnd
            if (passage.kind === "equal") {
                for (let line = first; result && line < end; ++line)
                    from[line] = (from[line] || 0) | (passage.side === "stage2" ? 1 : 2)
            } else if (passage.kind === (result ? "source-only" : "result-only")) {
                gaps[first] = true
            } else {
                mark(first, end, passage.kind === "changed" || passage.kind === "ambiguous" ? "changed" : "added")
            }
        }
        const regions = file.regions || []
        for (let i = 0; i < regions.length; ++i) {
            const region = regions[i]
            if (result && region.mapped) {
                if (region.state === "undecided")
                    mark(region.resultLineBegin, region.resultLineEnd, "conflict")
                else if (region.state === "manual")
                    mark(region.resultLineBegin, region.resultLineEnd, "manual")
                for (let line = region.resultLineBegin; region.handEdited && line < region.resultLineEnd; ++line)
                    from[line] = 4
            } else if (!result && region.side === pane && region.state === "undecided") {
                mark(region.sourceLineBegin, region.sourceLineEnd, "conflict")
            }
        }
        for (const line in from)
            letters[line] = ["", "L", "R", "B", "m"][from[line]]
        return {lines: lines, gaps: gaps, letters: letters}
    }
    // The lines of one pane that the navigation target covers, as [first, end)
    // ranges. An empty range marks the line boundary at first.
    function currentFor(pane: string, target: var, file: var): var {
        const ranges = []
        if (!target || target.fileId !== comparison.controller.selectedFileId || !(target.count > 0))
            return ranges
        let first = -1
        let end = -1
        if (target.scope === "result" && target.utf16Begin >= 0) {
            first = resultBlocks.lineAt(target.utf16Begin)
            end = target.utf16End > target.utf16Begin ? resultBlocks.lineAt(target.utf16End - 1) + 1 : first
        }
        if (pane === "result") {
            if (first > 0)
                ranges.push([first, end])
            return ranges
        }
        const regions = file.regions || []
        for (let i = 0; i < regions.length; ++i) {
            if (regions[i].id === target.contributionId && regions[i].side === pane) {
                ranges.push([regions[i].sourceLineBegin, regions[i].sourceLineEnd])
                return ranges
            }
        }
        if (first <= 0)
            return ranges
        // Map the result lines through this source's passages: equal lines
        // one to one, any other passage the range meets as a whole.
        const passages = file.passages || []
        for (let i = 0; i < passages.length; ++i) {
            const passage = passages[i]
            if (passage.side !== pane)
                continue
            const begin = passage.resultLineBegin
            const stop = passage.resultLineEnd
            const meets = stop > begin
                ? begin < Math.max(end, first + 1) && first < stop
                : begin >= first && begin <= end
            if (!meets)
                continue
            if (passage.kind === "equal") {
                const low = Math.max(first, begin)
                const high = Math.min(end, stop)
                ranges.push([passage.sourceLineBegin + low - begin, passage.sourceLineBegin + high - begin])
            } else {
                ranges.push([passage.sourceLineBegin, passage.sourceLineEnd])
            }
        }
        return ranges
    }

    // Scroll synchronisation. The panes share a reference row a few lines below
    // their tops. The line on the leading pane's row maps through its side's
    // passages to the result, and from there through each other side's
    // passages, and each other pane scrolls to put its line on that row. An
    // equal passage maps line by line; any other passage maps proportionally
    // between its boundaries, so an ambiguous one scrolls without a jump. A
    // side without passages maps proportionally over the whole text.
    property bool syncingScroll: false
    readonly property var scrollMaps: scrollMapsFor(file)
    function scrollMapsFor(file: var): var {
        const maps = {stage2: [], stage3: [], earlier: []}
        const passages = file.passages || []
        for (let i = 0; i < passages.length; ++i) {
            const passage = passages[i]
            if (maps[passage.side])
                maps[passage.side].push([passage.sourceLineBegin, passage.sourceLineEnd,
                    passage.resultLineBegin, passage.resultLineEnd])
        }
        return maps
    }
    // Maps a fractional line through one side's passages, from the source to
    // the result when toResult is set and back otherwise.
    function mapLine(map: var, line: real, toResult: bool, fromLines: int, toLines: int): real {
        if (map.length === 0)
            return fromLines > 0 ? 1 + (line - 1) * toLines / fromLines : 1
        const from = toResult ? 0 : 2
        const to = toResult ? 2 : 0
        // The first passage that ends below the line. Both sides are in order.
        let low = 0
        let high = map.length
        while (low < high) {
            const middle = (low + high) >> 1
            if (map[middle][from + 1] > line)
                high = middle
            else
                low = middle + 1
        }
        if (low === map.length) {
            const last = map[map.length - 1]
            return last[to + 1] + line - last[from + 1]
        }
        const passage = map[low]
        if (line <= passage[from])
            return passage[to]
        return passage[to] + (line - passage[from]) * (passage[to + 1] - passage[to])
            / (passage[from + 1] - passage[from])
    }
    function scrollPanes(): var {
        const panes = [
            {name: "stage2", viewport: leftPane.viewport, editor: leftPane.editor, blocks: leftPane.blocks},
            {name: "result", viewport: resultViewport, editor: resultEditor, blocks: resultBlocks},
            {name: "stage3", viewport: rightPane.viewport, editor: rightPane.editor, blocks: rightPane.blocks}]
        if (earlierPane.visible)
            panes.push({name: "earlier", viewport: earlierPane.viewport, editor: earlierPane.editor,
                blocks: earlierPane.blocks})
        return panes
    }
    // Up to three lines of context above the row, and at most a quarter of the shortest pane.
    function scrollRow(panes: var): real {
        let shortest = Infinity
        for (let i = 0; i < panes.length; ++i)
            shortest = Math.min(shortest, panes[i].viewport.height)
        return Math.max(0, Math.min(3 * paneFont.lineSpacing, shortest / 4))
    }
    function lineOnRow(pane: var, row: real): real {
        return pane.blocks.lineAtY(pane.viewport.contentY + row - pane.editor.y - pane.editor.topPadding)
    }
    function putLineOnRow(pane: var, line: real, row: real): void {
        const y = pane.editor.y + pane.editor.topPadding + pane.blocks.yAtLine(line) - row
        const bottom = Math.max(0, pane.viewport.contentHeight - pane.viewport.height)
        pane.viewport.contentY = Math.max(0, Math.min(bottom, y))
    }
    // Scrolls the other panes, or only the one named only, to follow leader.
    function followScroll(leader: string, only: string): void {
        const panes = scrollPanes()
        let lead = null
        for (let i = 0; i < panes.length; ++i) {
            if (panes[i].name === leader)
                lead = panes[i]
        }
        if (!lead || syncingScroll)
            return
        const row = scrollRow(panes)
        const resultLines = resultBlocks.blockCount
        const line = lineOnRow(lead, row)
        const resultLine = leader === "result" ? line
            : mapLine(scrollMaps[leader], line, true, lead.blocks.blockCount, resultLines)
        // Moving the other panes must not make them lead in turn.
        syncingScroll = true
        try {
            for (let i = 0; i < panes.length; ++i) {
                const pane = panes[i]
                if (pane === lead || (only.length > 0 && pane.name !== only))
                    continue
                putLineOnRow(pane, pane.name === "result" ? resultLine
                    : mapLine(scrollMaps[pane.name], resultLine, false, resultLines, pane.blocks.blockCount), row)
            }
        } finally {
            syncingScroll = false
        }
    }
    // A pane leads only when the user scrolls it: dragging, flicking or the
    // wheel, its scroll bar, or keyboard focus in its text. A relayout that
    // moves a pane the user is not scrolling leaves the others alone.
    function paneScrolled(name: string, byUser: bool): void {
        if (byUser)
            followScroll(name, "")
    }
    function followEarlier(): void {
        followScroll("result", "earlier")
    }
    // A newly selected file opens with the region the editor put its cursor on
    // at the reference row of every pane, or at the top without one. A
    // navigation in the file since then keeps its own position.
    function placePanes(): void {
        const target = controller.editorPlacement
        if (!target || target.fileId !== controller.selectedFileId)
            return
        const navigation = controller.navigationTarget
        if (navigation.fileId === target.fileId
                && (navigation.scope === "result" || navigation.scope === "unmapped-source"))
            return
        const panes = scrollPanes()
        let lead = panes[1]
        let line = 1
        // The result first, then a source for a region the result does not map.
        const order = [1, 0, 2]
        for (let i = 0; i < order.length; ++i) {
            const ranges = currentFor(panes[order[i]].name, target, file)
            if (ranges.length > 0) {
                lead = panes[order[i]]
                line = ranges[0][0]
                break
            }
        }
        const row = scrollRow(panes)
        putLineOnRow(lead, line, row)
        followScroll(lead.name, "")
        // A relayout scrolls a pane to keep its cursor in view, so keep each
        // source's cursor on the line it now shows rather than at the top. A
        // selection stays where the user made it.
        for (let i = 0; i < panes.length; ++i) {
            const editor = panes[i].editor
            if (panes[i].name !== "result" && editor.selectionStart === editor.selectionEnd)
                editor.cursorPosition = panes[i].blocks.positionOf(Math.floor(lineOnRow(panes[i], row)))
        }
    }
    function followNavigation(): void {
        const target = controller.navigationTarget
        if (target.fileId === controller.selectedFileId && target.scope === "result")
            followScroll("result", "")
    }
    Connections {
        target: comparison.controller
        function onEditorPlaced(): void { Qt.callLater(comparison.placePanes) }
        function onNavigationChanged(): void { Qt.callLater(comparison.followNavigation) }
    }
    TextBlocks {
        id: resultBlocks
        document: resultEditor.textDocument
    }
    Component.onCompleted: {
        controller.setSyntaxBackground(schemeBackground)
        refreshSearchScopes()
        applyWhitespace()
        whitespaceReady = true
        Qt.callLater(placePanes)
    }
    onSchemeBackgroundChanged: controller.setSyntaxBackground(schemeBackground)

    // Qt Quick ignores QTextOption.ShowTabsAndSpaces, so the markers are drawn
    // over the text. They are not document characters.
    component WhitespaceMarkers: Item {
        id: markers
        required property TextEdit editor
        required property Flickable viewport
        property bool active: false
        property bool wrapped: false
        property var spots: []
        readonly property int markerCount: spots.length
        readonly property color ink: editor.color.a > 0 ? editor.color : Kirigami.Theme.textColor
        readonly property real stroke: Kirigami.Units.smallSpacing / 2
        readonly property real inset: Kirigami.Units.smallSpacing / 4
        z: 2
        enabled: false
        clip: true

        property bool refreshQueued: false
        function schedule(): void {
            if (refreshQueued)
                return
            refreshQueued = true
            Qt.callLater(markers.refresh)
        }
        function refresh(): void {
            refreshQueued = false
            if (!active || width <= 0 || height <= 0) {
                spots = []
                return
            }
            const text = editor.text || ""
            const found = []
            if (text.length === 0) {
                spots = found
                return
            }
            const origin = editor.mapFromItem(markers, 0, 0)
            let pos = editor.positionAt(origin.x, Math.max(0, origin.y))
            if (pos < 0)
                pos = 0
            while (pos > 0) {
                const prev = text.charAt(pos - 1)
                if (prev === "\n" || prev === "\u2028" || prev === "\u2029")
                    break
                pos = pos - 1
            }
            let guard = 0
            while (pos < text.length && guard < 200000) {
                guard = guard + 1
                const ch = text.charAt(pos)
                const rect = editor.positionToRectangle(pos)
                const mapped = editor.mapToItem(markers, rect.x, rect.y)
                if (mapped.y > height + rect.height)
                    break
                let span = rect.width
                if (pos + 1 <= text.length) {
                    const next = editor.positionToRectangle(pos + 1)
                    const mappedNext = editor.mapToItem(markers, next.x, next.y)
                    if (Math.abs(mappedNext.y - mapped.y) < rect.height / 2)
                        span = mappedNext.x - mapped.x
                }
                if (span < markers.stroke)
                    span = ch === "\t" ? Kirigami.Units.largeSpacing : markers.stroke
                if ((ch === " " || ch === "\t") && mapped.y + rect.height >= 0
                        && mapped.x < width && mapped.x + span > 0)
                    found.push({x: mapped.x, y: mapped.y, w: span, h: Math.max(rect.height, markers.stroke), tab: ch === "\t"})
                if (mapped.x > width && !wrapped) {
                    const newline = text.indexOf("\n", pos)
                    if (newline < 0)
                        break
                    pos = newline + 1
                    continue
                }
                pos = pos + 1
            }
            spots = found
        }
        onActiveChanged: schedule()
        Component.onCompleted: schedule()
        Connections {
            target: markers.editor
            function onTextChanged(): void { markers.schedule() }
            function onWidthChanged(): void { markers.schedule() }
            function onHeightChanged(): void { markers.schedule() }
        }
        Connections {
            target: markers.viewport
            function onContentXChanged(): void { markers.schedule() }
            function onContentYChanged(): void { markers.schedule() }
            function onWidthChanged(): void { markers.schedule() }
            function onHeightChanged(): void { markers.schedule() }
        }
        Repeater {
            model: markers.spots
            delegate: Item {
                id: spot
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: Math.max(markers.stroke, modelData.w)
                height: Math.max(markers.stroke, modelData.h)
                Rectangle {
                    visible: !spot.modelData.tab
                    width: Math.min(Kirigami.Units.smallSpacing, Math.max(markers.stroke, spot.height * 0.22))
                    height: width
                    radius: width / 2
                    color: markers.ink
                    anchors.centerIn: parent
                }
                Rectangle {
                    visible: spot.modelData.tab
                    x: markers.inset
                    height: markers.stroke
                    width: Math.max(markers.inset, parent.width - Kirigami.Units.largeSpacing)
                    anchors.verticalCenter: parent.verticalCenter
                    color: markers.ink
                }
                Rectangle {
                    visible: spot.modelData.tab
                    width: Kirigami.Units.smallSpacing + markers.inset
                    height: width
                    radius: markers.inset
                    anchors.right: parent.right
                    anchors.rightMargin: markers.inset
                    anchors.verticalCenter: parent.verticalCenter
                    color: markers.ink
                }
            }
        }
    }

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
    // slot shows the letter in marks.letters.
    component LineGutter: Item {
        id: gutter
        required property TextArea editor
        required property Flickable viewport
        property var rows: []
        property var marks: ({lines: {}, gaps: {}, letters: {}})
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
            if (!same)
                rows = next
        }
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
            model: gutter.rows
            delegate: Text {
                required property var modelData
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
            model: gutter.provenance ? gutter.rows : []
            delegate: Text {
                required property var modelData
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
                model: gutter.rows
                delegate: Text {
                    required property var modelData
                    readonly property int line: modelData.number
                    readonly property string kind: gutter.marks.lines[line] || ""
                    y: modelData.y
                    height: modelData.height
                    text: ({added: "+", changed: "~", conflict: "!", manual: "m"})[kind] || ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    font: gutter.editor.font
                    color: kind === "added" ? Kirigami.Theme.positiveTextColor
                        : kind === "changed" ? Kirigami.Theme.activeTextColor
                        : kind === "conflict" ? Kirigami.Theme.negativeTextColor
                        : Kirigami.Theme.neutralTextColor
                }
            }
            Repeater {
                model: gutter.rows
                delegate: Text {
                    required property var modelData
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

    // Diff colours for one pane, painted under its text rather than as
    // character formats. The layer sits in the pane's Flickable below the
    // TextArea, takes its rows and marks from the pane's gutter and scrolls
    // with the text. A gap is a thin rule on a line boundary. current lists
    // the [first, end) line ranges of the navigation target, drawn on top.
    component LineBackgrounds: Item {
        id: backgrounds
        required property LineGutter gutter
        property var current: []
        readonly property real rule: Kirigami.Units.smallSpacing / 2
        z: -1
        enabled: false
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
        function isCurrent(line: int): bool {
            for (let i = 0; i < current.length; ++i) {
                if (line >= current[i][0] && line < current[i][1])
                    return true
            }
            return false
        }
        function isCurrentGap(line: int): bool {
            for (let i = 0; i < current.length; ++i) {
                if (current[i][0] === line && current[i][1] === line)
                    return true
            }
            return false
        }
        Repeater {
            model: backgrounds.gutter.rows
            delegate: Rectangle {
                required property var modelData
                readonly property string part: "line"
                readonly property int line: modelData.number
                readonly property string kind: backgrounds.gutter.marks.lines[line] || ""
                visible: kind.length > 0
                y: modelData.top
                width: backgrounds.width
                height: modelData.extent
                color: kind === "added" ? Kirigami.Theme.positiveBackgroundColor
                    : kind === "changed" ? Kirigami.Theme.activeBackgroundColor
                    : kind === "conflict" ? Kirigami.Theme.negativeBackgroundColor
                    : Kirigami.Theme.neutralBackgroundColor
            }
        }
        Repeater {
            model: backgrounds.gutter.rows
            delegate: Rectangle {
                required property var modelData
                readonly property string part: "gap"
                readonly property int line: modelData.number
                readonly property string kind: backgrounds.gutter.marks.gaps[line] ? "removed" : ""
                visible: kind.length > 0
                y: modelData.top - height / 2
                width: backgrounds.width
                height: backgrounds.rule
                color: Kirigami.Theme.disabledTextColor
            }
        }
        Repeater {
            model: backgrounds.gutter.rows
            delegate: Rectangle {
                required property var modelData
                readonly property string part: "current"
                readonly property int line: modelData.number
                readonly property bool gap: backgrounds.isCurrentGap(line)
                visible: gap || backgrounds.isCurrent(line)
                y: gap ? modelData.top - height / 2 : modelData.top
                width: backgrounds.width
                height: gap ? backgrounds.rule * 2 : modelData.extent
                color: Kirigami.Theme.highlightColor
                opacity: 0.3
            }
        }
    }

    function paneMinimumHeight(lines: int): real {
        return Math.ceil(lines * paneFont.lineSpacing + resultEditor.topPadding + resultEditor.bottomPadding)
    }
    FontMetrics {
        id: paneFont
        font: resultEditor.font
    }
    // Four characters, so tab-indented text lines up the same in every pane.
    readonly property real tabStop: 4 * paneFont.averageCharacterWidth

    component CapturedSource: ColumnLayout {
        id: sourcePane
        required property string paneName
        required property string paneRole
        required property var source
        property alias editor: sourceText
        property alias viewport: sourceViewport
        property alias blocks: sourceBlocks
        property bool alignHeader: true
        property int minimumLines: comparison.minimumPaneLines
        readonly property real headerImplicitHeight: sourceHeaderRow.implicitHeight
        // The commit id is cut to 10 characters on the line; the tooltip keeps the full ids.
        readonly property string commitId: sourcePane.source.commitId || ""
        readonly property string fullIdentity: qsTr("Verified commit: %1\nCaptured blob: %2")
            .arg(sourcePane.commitId || qsTr("unavailable"))
            .arg(sourcePane.source.objectId || qsTr("unavailable"))
        function shortCommit(label: string): string {
            return sourcePane.commitId.length > 10
                ? label.replace(sourcePane.commitId, sourcePane.commitId.slice(0, 10)) : label
        }
        RowLayout {
            id: sourceHeaderRow
            Layout.fillWidth: true
            Layout.preferredHeight: sourcePane.alignHeader ? comparison.headerHeight : implicitHeight
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
            Layout.minimumHeight: comparison.paneMinimumHeight(sourcePane.minimumLines)
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
                onContentYChanged: comparison.paneScrolled(sourcePane.paneName,
                    moving || !!sourceBar && sourceBar.pressed || sourceText.activeFocus)
                TextArea.flickable: TextArea {
                    id: sourceText
                    objectName: sourcePane.paneName + "Text"
                    text: sourcePane.source.text || ""
                    readOnly: true
                    selectByMouse: true
                    persistentSelection: true
                    textFormat: TextEdit.PlainText
                    font: Kirigami.Theme.fixedWidthFont
                    tabStopDistance: comparison.tabStop
                    leftPadding: sourceGutter.width + Kirigami.Units.smallSpacing
                    width: comparison.wrapLines ? sourceViewport.width : implicitWidth
                    wrapMode: comparison.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                    Accessible.name: sourcePane.paneRole + " · " + (sourcePane.source.label || "")
                    onTextChanged: if (comparison.whitespaceReady)
                        comparison.applyWhitespace()
                    Component.onCompleted: comparison.controller.attachSourceEditor(sourceText)
                }
                LineGutter {
                    id: sourceGutter
                    objectName: sourcePane.paneName + "Gutter"
                    x: sourceViewport.contentX
                    width: implicitWidth
                    height: Math.max(sourceText.height, sourceViewport.height)
                    editor: sourceText
                    viewport: sourceViewport
                    marks: comparison.marksFor(sourcePane.paneName, comparison.file)
                }
                LineBackgrounds {
                    objectName: sourcePane.paneName + "Backgrounds"
                    width: Math.max(sourceText.width, sourceViewport.width)
                    height: sourceGutter.height
                    gutter: sourceGutter
                    current: comparison.currentFor(sourcePane.paneName, comparison.controller.navigationTarget,
                        comparison.file)
                }
            }
            WhitespaceMarkers {
                objectName: sourcePane.paneName + "Whitespace"
                anchors.fill: parent
                anchors.leftMargin: sourceGutter.width
                editor: sourceText
                viewport: sourceViewport
                active: comparison.showWhitespace
                wrapped: comparison.wrapLines
            }
        }
        TextBlocks {
            id: sourceBlocks
            document: sourceText.textDocument
        }
    }

    ColumnLayout {
        objectName: "readingControls"
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        RowLayout {
            Layout.fillWidth: true
            ComboBox {
                id: searchScope
                objectName: "searchScope"
                model: [qsTr("Left source"), qsTr("Editable result"), qsTr("Right source")]
                Accessible.name: qsTr("Search scope")
                Layout.preferredWidth: Math.max(implicitWidth, Kirigami.Units.gridUnit * 10)
                onCurrentIndexChanged: comparison.searchDetail = ""
            }
            TextField {
                id: searchQuery
                objectName: "searchQuery"
                Layout.fillWidth: true
                placeholderText: qsTr("Find a passage")
                Accessible.name: qsTr("Passage search")
                onAccepted: comparison.findPassage()
            }
            Button {
                objectName: "findPassage"
                text: qsTr("Find")
                onClicked: comparison.findPassage()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            CheckBox {
                id: wrapLinesBox
                objectName: "wrapLines"
                text: qsTr("Wrap lines")
            }
            CheckBox {
                id: showWhitespaceBox
                objectName: "showWhitespace"
                text: qsTr("Show spaces and tabs")
                onToggled: comparison.applyWhitespace()
            }
            Label {
                objectName: "searchStatus"
                Layout.fillWidth: true
                text: comparison.searchStatusText
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
        }
    }
    SplitView {
        id: panes
        objectName: "comparisonPanes"
        Layout.fillWidth: true
        Layout.fillHeight: true
        // A SplitView does not pass its panes' minimum height on to the layout.
        Layout.minimumHeight: Math.max(leftPane.Layout.minimumHeight, resultColumn.Layout.minimumHeight,
            rightPane.Layout.minimumHeight)
        orientation: Qt.Horizontal
        CapturedSource {
            id: leftPane
            SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
            SplitView.preferredWidth: panes.width / 3
            paneName: "stage2"
            paneRole: qsTr("Left source")
            source: comparison.file.stage2 || ({})
        }
        ColumnLayout {
            id: resultColumn
            SplitView.minimumWidth: Kirigami.Units.gridUnit * 10
            SplitView.preferredWidth: panes.width / 3
            Label {
                id: resultHeader
                objectName: "resultLabel"
                Layout.fillWidth: true
                Layout.preferredHeight: comparison.headerHeight
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
                Layout.minimumHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
                Flickable {
                    id: resultViewport
                    objectName: "resultViewport"
                    anchors.fill: parent
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.horizontal: ScrollBar {}
                    ScrollBar.vertical: ScrollBar {
                        id: resultBar
                    }
                    onContentYChanged: comparison.paneScrolled("result",
                        moving || !!resultBar && resultBar.pressed || resultEditor.activeFocus)
                    TextArea.flickable: TextArea {
                        id: resultEditor
                        objectName: "resultText"
                        // Only the adapter enables editing after loading durable captures.
                        readOnly: true
                        selectByMouse: true
                        persistentSelection: true
                        textFormat: TextEdit.PlainText
                        font: Kirigami.Theme.fixedWidthFont
                        tabStopDistance: comparison.tabStop
                        leftPadding: resultGutter.width + Kirigami.Units.smallSpacing
                        width: comparison.wrapLines ? resultViewport.width : implicitWidth
                        wrapMode: comparison.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                        Accessible.name: qsTr("Editable result")
                        Component.onCompleted: comparison.controller.attachResultEditor(resultEditor)
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: resultMenu.popup()
                        }
                        Menu {
                            id: resultMenu
                            MenuItem {
                                text: qsTr("Undo")
                                enabled: !!comparison.file.canEdit && !!comparison.file.canUndo && !comparison.inputMethodComposing
                                onTriggered: comparison.controller.undo(comparison.controller.selectedFileId, comparison.file.currentRevision)
                            }
                            MenuItem {
                                text: qsTr("Redo")
                                enabled: !!comparison.file.canEdit && !!comparison.file.canRedo && !comparison.inputMethodComposing
                                onTriggered: comparison.controller.redo(comparison.controller.selectedFileId, comparison.file.currentRevision)
                            }
                            MenuSeparator {}
                            MenuItem {
                                text: qsTr("Cut")
                                enabled: !resultEditor.readOnly && resultEditor.selectedText.length > 0 && !comparison.inputMethodComposing
                                onTriggered: comparison.controller.resultClipboard("cut")
                            }
                            MenuItem {
                                text: qsTr("Copy")
                                enabled: resultEditor.selectedText.length > 0
                                onTriggered: comparison.controller.resultClipboard("copy")
                            }
                            MenuItem {
                                text: qsTr("Paste")
                                enabled: !resultEditor.readOnly && resultEditor.canPaste && !comparison.inputMethodComposing
                                onTriggered: comparison.controller.resultClipboard("paste")
                            }
                            MenuItem {
                                text: qsTr("Select all")
                                onTriggered: comparison.controller.resultClipboard("selectAll")
                            }
                        }
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
                        marks: comparison.marksFor("result", comparison.file)
                    }
                    LineBackgrounds {
                        objectName: "resultBackgrounds"
                        width: Math.max(resultEditor.width, resultViewport.width)
                        height: resultGutter.height
                        gutter: resultGutter
                        current: comparison.currentFor("result", comparison.controller.navigationTarget,
                            comparison.file)
                    }
                }
                WhitespaceMarkers {
                    objectName: "resultWhitespace"
                    anchors.fill: parent
                    anchors.leftMargin: resultGutter.width
                    editor: resultEditor
                    viewport: resultViewport
                    active: comparison.showWhitespace
                    wrapped: comparison.wrapLines
                }
            }
        }
        CapturedSource {
            id: rightPane
            SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
            SplitView.preferredWidth: panes.width / 3
            paneName: "stage3"
            paneRole: qsTr("Right source")
            source: comparison.file.stage3 || ({})
        }
    }
    CheckBox {
        id: showEarlier
        objectName: "showEarlier"
        enabled: !!comparison.file.earlier && !!comparison.file.earlier.available
        text: enabled ? qsTr("Show common earlier version") : qsTr("Common earlier version unavailable")
        onCheckedChanged: comparison.refreshSearchScopes()
    }
    CapturedSource {
        id: earlierPane
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(comparison.height / 3, Kirigami.Units.gridUnit * 10)
        visible: showEarlier.enabled && showEarlier.checked
        alignHeader: false
        minimumLines: 3
        paneName: "earlier"
        paneRole: qsTr("Earlier-version context · read-only")
        source: comparison.file.earlier || ({})
        onVisibleChanged: if (visible)
            Qt.callLater(comparison.followEarlier)
    }
}
