// The comparison of one file: the reading controls, the source and result panes with the
// connector bands between them, and the earlier version below.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: comparison
    required property var controller
    required property var file
    readonly property bool inputMethodComposing: resultPane.editor.inputMethodComposing
    readonly property real headerHeight: Math.max(leftPane.headerImplicitHeight,
        resultPane.headerImplicitHeight, rightPane.headerImplicitHeight)
    readonly property bool wrapLines: readingBar.wrapLines
    readonly property bool showWhitespace: readingBar.showWhitespace
    property var scopeNames: ["stage2", "result", "stage3"]
    property string searchDetail: ""
    // Text panes never get shorter than this many lines; on a short window the page scrolls instead.
    property int minimumPaneLines: 8
    property bool whitespaceReady: false
    // Shows the raw lines of every conflict-marker block instead of folding them.
    property bool showMarkers: false
    // Asks for showMarkers, because something the user asked for is on a folded line.
    signal markersRevealed()
    readonly property var markerFold: kept("fold", foldFor(file, showMarkers))
    // The controller picks Breeze Light or Breeze Dark syntax colours from its lightness.
    readonly property color schemeBackground: Kirigami.Theme.backgroundColor
    readonly property string searchStatusText: searchDetail.length > 0
        ? searchDetail
        : qsTr("Search scope: %1.").arg(readingBar.scope.currentText || qsTr("Editable result"))

    // The last value of each kept binding, with its JSON text.
    readonly property var keptValues: ({})
    // Each revision gives the file a new map, so the marks, ranges, bands and fold made from it
    // are new objects too. One equal to the last value under key is that value instead, so the
    // panes and their line delegates are not told of a change when typing leaves it the same.
    function kept(key: string, value: var): var {
        const text = JSON.stringify(value)
        const last = keptValues[key]
        if (last && last.text === text)
            return last.value
        keptValues[key] = {text: text, value: value}
        return value
    }
    function editorNamed(name): Item {
        if (name === "stage2")
            return leftPane.editor
        if (name === "stage3")
            return rightPane.editor
        if (name === "earlier")
            return earlierPane.editor
        return resultPane.editor
    }
    function refreshSearchScopes(): void {
        const labels = [qsTr("Left source"), qsTr("Editable result"), qsTr("Right source")]
        const names = ["stage2", "result", "stage3"]
        if (showEarlier.enabled && showEarlier.checked) {
            labels.push(qsTr("Earlier version"))
            names.push("earlier")
        }
        const current = scopeNames[readingBar.scope.currentIndex] || "result"
        scopeNames = names
        readingBar.scope.model = labels
        const next = names.indexOf(current)
        readingBar.scope.currentIndex = next >= 0 ? next : names.indexOf("result")
    }
    function applyWhitespace(): void {
        const visible = readingBar.showWhitespace
        const editors = [leftPane.editor, resultPane.editor, rightPane.editor, earlierPane.editor]
        for (let i = 0; i < editors.length; ++i)
            controller.setVisibleWhitespace(editors[i], visible)
    }
    // A source's text is replaced when its file changes; its markers follow once set up.
    function sourceTextReplaced(): void {
        if (whitespaceReady)
            applyWhitespace()
    }
    function findPassage(): void {
        const index = readingBar.scope.currentIndex
        const name = index >= 0 && index < scopeNames.length ? scopeNames[index] : "result"
        const editor = editorNamed(name)
        const scope = readingBar.scope.currentText || qsTr("Editable result")
        const query = readingBar.query
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
        if (editor === resultPane.editor && resultPane.blocks.hiddenWithin(found, found + query.length))
            markersRevealed()
        editor.select(found, found + query.length)
        followScroll(name, "")
        searchDetail = qsTr("Passage in %1.").arg(scope)
    }
    // Line marks for one pane, keyed by 1-based line. lines holds the kind of
    // each coloured line: "added" when the compared text lacks it, "changed",
    // "conflict" inside an undecided region, "manual" inside a hand-edited
    // result region, "accepted" and "ignored" inside a decided region of a
    // source pane, or "markers" on the lines of a conflict-marker block or a
    // stray marker line of the result. gaps holds the lines above which the
    // compared text has lines this pane lacks. letters holds the result's
    // provenance: L, R or B for a line equal to the left, right or both
    // sources, m for a hand edit.
    function marksFor(pane: string, file: var): var {
        const lines = {}
        const gaps = {}
        const letters = {}
        const rank = {added: 1, changed: 2, accepted: 3, ignored: 3, manual: 3, conflict: 4, markers: 5}
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
            } else if (!result && region.side === pane && region.state !== "manual") {
                mark(region.sourceLineBegin, region.sourceLineEnd,
                    region.state === "undecided" ? "conflict" : region.state)
            }
        }
        if (result) {
            const blocks = file.markerBlocks || []
            for (let i = 0; i < blocks.length; ++i)
                mark(blocks[i].resultLineBegin, blocks[i].resultLineEnd, "markers")
            const orphans = file.markerOrphans || []
            for (let i = 0; i < orphans.length; ++i)
                mark(orphans[i].line, orphans[i].line + 1, "markers")
        }
        for (const line in from)
            letters[line] = ["", "L", "R", "B", "m"][from[line]]
        return {lines: lines, gaps: gaps, letters: letters}
    }
    // The result's folded marker lines: those of every block tied to a choice
    // section, unless the raw markers are shown. The buffer keeps them. hidden
    // lists them as {line, text} for TextBlocks. boundaries maps the first line
    // below each run of hidden lines to the run's size, and sets chip on a run
    // that opens a block, where the gutter says that markers remain.
    function foldFor(file: var, shown: bool): var {
        const hidden = []
        const opening = {}
        const blocks = shown ? [] : file.markerBlocks || []
        for (let i = 0; i < blocks.length; ++i) {
            const lines = blocks[i].markerLines
            if (!blocks[i].folded || lines.length === 0)
                continue
            opening[lines[0].line] = true
            for (let j = 0; j < lines.length; ++j)
                hidden.push(lines[j])
        }
        hidden.sort((a, b) => a.line - b.line)
        const boundaries = {}
        for (let first = 0; first < hidden.length;) {
            let last = first
            while (last + 1 < hidden.length && hidden[last + 1].line === hidden[last].line + 1)
                ++last
            let chip = false
            for (let i = first; i <= last; ++i)
                chip = chip || !!opening[hidden[i].line]
            boundaries[hidden[last].line + 1] = {count: last - first + 1, chip: chip}
            first = last + 1
        }
        return {hidden: hidden, boundaries: boundaries}
    }
    // The connector bands between one source and the result: one per
    // non-equal passage of that side, in order, with its source and result
    // lines as [begin, end) ranges. The kind follows the passage: the result
    // changes the lines, adds a result-only passage's or drops a source-only
    // one's. The state of a region of that side on the source lines, or on
    // the boundary of an empty passage, outranks it as it does in the panes,
    // and a passage that meets a conflict-marker block of the result is a
    // conflict, so the band covers the block's folded marker lines.
    function bandsFor(side: string, file: var): var {
        const rank = {removed: 0, added: 1, changed: 2, accepted: 3, ignored: 3, manual: 3, conflict: 4}
        const lines = {}
        const boundaries = {}
        const decide = function(states, line, kind) {
            if (!states[line] || rank[states[line]] < rank[kind])
                states[line] = kind
        }
        const regions = file.regions || []
        for (let i = 0; i < regions.length; ++i) {
            const region = regions[i]
            if (region.side !== side)
                continue
            const kind = region.state === "undecided" ? "conflict" : region.state
            if (region.sourceLineBegin === region.sourceLineEnd)
                decide(boundaries, region.sourceLineBegin, kind)
            for (let line = region.sourceLineBegin; line < region.sourceLineEnd; ++line)
                decide(lines, line, kind)
        }
        const marked = file.markerBlocks || []
        const inBlock = function(begin, end) {
            for (let i = 0; i < marked.length; ++i) {
                const first = marked[i].resultLineBegin
                const stop = marked[i].resultLineEnd
                if (begin < end ? begin < stop && first < end : begin > first && begin < stop)
                    return true
            }
            return false
        }
        const bands = []
        const passages = file.passages || []
        for (let i = 0; i < passages.length; ++i) {
            const passage = passages[i]
            if (passage.side !== side || passage.kind === "equal")
                continue
            const begin = passage.sourceLineBegin
            const end = passage.sourceLineEnd
            const states = begin === end ? boundaries : lines
            let kind = passage.kind === "result-only" ? "added" : passage.kind === "source-only" ? "removed" : "changed"
            for (let line = begin; line < Math.max(end, begin + 1); ++line) {
                const state = states[line]
                if (state && rank[state] > rank[kind])
                    kind = state
            }
            if (rank[kind] < rank.conflict && inBlock(passage.resultLineBegin, passage.resultLineEnd))
                kind = "conflict"
            bands.push({kind: kind, sourceBegin: begin, sourceEnd: end,
                resultBegin: passage.resultLineBegin, resultEnd: passage.resultLineEnd})
        }
        return bands
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
            first = resultPane.blocks.lineAt(target.utf16Begin)
            end = target.utf16End > target.utf16Begin ? resultPane.blocks.lineAt(target.utf16End - 1) + 1 : first
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
            {name: "result", viewport: resultPane.viewport, editor: resultPane.editor, blocks: resultPane.blocks},
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
        const resultLines = resultPane.blocks.blockCount
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
    // A relayout scrolls a source to keep its cursor in view, so keep the
    // cursor on a line the source now shows rather than at the top. A
    // selection stays where the user made it.
    function keepCursorOnLine(pane: var, line: real): void {
        const editor = pane.editor
        if (editor.selectionStart === editor.selectionEnd)
            editor.cursorPosition = pane.blocks.positionOf(line)
    }
    // Showing the earlier version lays it out again, which would scroll it
    // back to the cursor it had while hidden. Following can leave a line cut
    // at the row, so the cursor goes to the first whole line below it.
    function followEarlier(): void {
        const panes = scrollPanes()
        const earlier = panes.find(pane => pane.name === "earlier")
        if (!earlier)
            return
        followScroll("result", "earlier")
        keepCursorOnLine(earlier, Math.ceil(lineOnRow(earlier, scrollRow(panes))))
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
        for (let i = 0; i < panes.length; ++i) {
            if (panes[i].name !== "result")
                keepCursorOnLine(panes[i], Math.floor(lineOnRow(panes[i], row)))
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
    // The result's cursor never rests on a folded line. A key that moves it
    // there takes it on in the key's direction, keeping its column for a move
    // by lines; a selection set by other code ends on the side of its anchor;
    // a cursor placed by other code goes to the next shown line. The check
    // runs after the change completes, because an edit that joins a folded
    // line to its neighbour shows that line once the result is captured.
    property int cursorKey: 0
    property int settledCursor: 0
    property bool cursorCheckQueued: false
    // Shows the markers when the cursor is on a folded line, as after going to a marker.
    function revealCursorLine(): void {
        if (resultPane.blocks.hiddenAt(resultPane.editor.cursorPosition))
            markersRevealed()
    }
    function queueCursorCheck(): void {
        if (cursorCheckQueued)
            return
        cursorCheckQueued = true
        Qt.callLater(comparison.skipFoldedLines)
    }
    function skipFoldedLines(): void {
        cursorCheckQueued = false
        const key = cursorKey
        cursorKey = 0
        const editor = resultPane.editor
        const position = editor.cursorPosition
        if (resultPane.blocks.hiddenAt(position)) {
            const selecting = editor.selectionStart !== editor.selectionEnd
            const anchor = editor.selectionStart === position ? editor.selectionEnd : editor.selectionStart
            const byLines = [Qt.Key_Up, Qt.Key_Down, Qt.Key_PageUp, Qt.Key_PageDown].indexOf(key) >= 0
            const forward = key === Qt.Key_Down || key === Qt.Key_PageDown ? true
                : key === Qt.Key_Up || key === Qt.Key_PageUp ? false
                : key !== 0 ? position > settledCursor
                : selecting ? anchor > position : true
            const target = resultPane.blocks.shownPosition(position, forward, byLines ? settledCursor : -1)
            if (selecting)
                editor.moveCursorSelection(target, TextEdit.SelectCharacters)
            else
                editor.cursorPosition = target
        }
        settledCursor = editor.cursorPosition
    }
    Component.onCompleted: {
        controller.setSyntaxBackground(schemeBackground)
        refreshSearchScopes()
        applyWhitespace()
        whitespaceReady = true
        Qt.callLater(placePanes)
    }
    onSchemeBackgroundChanged: controller.setSyntaxBackground(schemeBackground)

    function paneMinimumHeight(lines: int): real {
        return Math.ceil(lines * paneFont.lineSpacing + resultPane.editor.topPadding
            + resultPane.editor.bottomPadding)
    }
    FontMetrics {
        id: paneFont
        font: resultPane.editor.font
    }
    // Four characters, so tab-indented text lines up the same in every pane.
    readonly property real tabStop: 4 * paneFont.averageCharacterWidth

    ReadingBar {
        id: readingBar
        Layout.fillWidth: true
        statusText: comparison.searchStatusText
        onScopeSwitched: comparison.searchDetail = ""
        onFindRequested: comparison.findPassage()
        onWhitespaceToggled: comparison.applyWhitespace()
    }
    SplitView {
        id: panes
        objectName: "comparisonPanes"
        Layout.fillWidth: true
        Layout.fillHeight: true
        // A SplitView does not pass its panes' minimum height on to the layout.
        Layout.minimumHeight: Math.max(leftPane.Layout.minimumHeight, resultPane.Layout.minimumHeight,
            rightPane.Layout.minimumHeight)
        orientation: Qt.Horizontal
        handle: ConnectorHandle {
            id: connectorHandle
            result: resultPane
            source: connectorHandle.leftSide ? leftPane : rightPane
            bands: comparison.kept("bands:" + connectorHandle.source.paneName,
                comparison.bandsFor(connectorHandle.source.paneName, comparison.file))
        }
        SourcePane {
            id: leftPane
            SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
            SplitView.preferredWidth: panes.width / 3
            paneName: "stage2"
            paneRole: qsTr("Left source")
            source: comparison.file.stage2 || ({})
            marks: comparison.kept("marks:stage2", comparison.marksFor(leftPane.paneName, comparison.file))
            current: comparison.kept("current:stage2", comparison.currentFor(leftPane.paneName,
                comparison.controller.navigationTarget, comparison.file))
            headerHeight: comparison.headerHeight
            tabStop: comparison.tabStop
            wrapLines: comparison.wrapLines
            showWhitespace: comparison.showWhitespace
            minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
            onScrolled: byUser => comparison.paneScrolled(leftPane.paneName, byUser)
            onTextReplaced: comparison.sourceTextReplaced()
            onEditorCompleted: comparison.controller.attachSourceEditor(leftPane.editor)
        }
        ResultPane {
            id: resultPane
            SplitView.minimumWidth: Kirigami.Units.gridUnit * 10
            SplitView.preferredWidth: panes.width / 3
            controller: comparison.controller
            file: comparison.file
            marks: comparison.kept("marks:result", comparison.marksFor("result", comparison.file))
            current: comparison.kept("current:result", comparison.currentFor("result",
                comparison.controller.navigationTarget, comparison.file))
            fold: comparison.markerFold
            headerHeight: comparison.headerHeight
            tabStop: comparison.tabStop
            wrapLines: comparison.wrapLines
            showWhitespace: comparison.showWhitespace
            minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
            onScrolled: byUser => comparison.paneScrolled("result", byUser)
            onCursorMoved: comparison.queueCursorCheck()
            onKeyPressed: key => {
                comparison.cursorKey = key
                comparison.queueCursorCheck()
            }
            // A fold can hide the line the cursor is on.
            onLinesLaidOut: comparison.queueCursorCheck()
        }
        SourcePane {
            id: rightPane
            SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
            SplitView.preferredWidth: panes.width / 3
            paneName: "stage3"
            paneRole: qsTr("Right source")
            source: comparison.file.stage3 || ({})
            marks: comparison.kept("marks:stage3", comparison.marksFor(rightPane.paneName, comparison.file))
            current: comparison.kept("current:stage3", comparison.currentFor(rightPane.paneName,
                comparison.controller.navigationTarget, comparison.file))
            headerHeight: comparison.headerHeight
            tabStop: comparison.tabStop
            wrapLines: comparison.wrapLines
            showWhitespace: comparison.showWhitespace
            minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
            onScrolled: byUser => comparison.paneScrolled(rightPane.paneName, byUser)
            onTextReplaced: comparison.sourceTextReplaced()
            onEditorCompleted: comparison.controller.attachSourceEditor(rightPane.editor)
        }
    }
    CheckBox {
        id: showEarlier
        objectName: "showEarlier"
        enabled: !!comparison.file.earlier && !!comparison.file.earlier.available
        text: enabled ? qsTr("Show common earlier version") : qsTr("Common earlier version unavailable")
        onCheckedChanged: comparison.refreshSearchScopes()
    }
    SourcePane {
        id: earlierPane
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(comparison.height / 3, Kirigami.Units.gridUnit * 10)
        visible: showEarlier.enabled && showEarlier.checked
        alignHeader: false
        paneName: "earlier"
        paneRole: qsTr("Earlier-version context · read-only")
        source: comparison.file.earlier || ({})
        marks: comparison.kept("marks:earlier", comparison.marksFor(earlierPane.paneName, comparison.file))
        current: comparison.kept("current:earlier", comparison.currentFor(earlierPane.paneName,
            comparison.controller.navigationTarget, comparison.file))
        headerHeight: comparison.headerHeight
        tabStop: comparison.tabStop
        wrapLines: comparison.wrapLines
        showWhitespace: comparison.showWhitespace
        minimumTextHeight: comparison.paneMinimumHeight(3)
        onScrolled: byUser => comparison.paneScrolled(earlierPane.paneName, byUser)
        onTextReplaced: comparison.sourceTextReplaced()
        onEditorCompleted: comparison.controller.attachSourceEditor(earlierPane.editor)
        onVisibleChanged: if (visible)
            Qt.callLater(comparison.followEarlier)
    }
}
