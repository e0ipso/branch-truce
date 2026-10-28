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
    // Text panes never get shorter than this many lines; on a short window the page scrolls instead.
    property int minimumPaneLines: 8
    // Shows the raw lines of every conflict-marker block instead of folding them.
    property bool showMarkers: false
    // Asks for showMarkers, because something the user asked for is on a folded line.
    signal markersRevealed()
    readonly property var markerFold: kept("fold", foldFor(file, showMarkers))
    // The controller picks Breeze Light or Breeze Dark syntax colours from its lightness.
    readonly property color schemeBackground: Kirigami.Theme.backgroundColor

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
    function editorNamed(name: string): Item {
        return panesByRole.editor(name)
    }
    // Selects the next passage the search finds and scrolls the panes to it, showing the
    // folded markers when it lies on them.
    function findPassage(): void {
        const name = passageSearch.scope
        const match = passageSearch.find(name, readingBar.query, true)
        if (match.begin < 0)
            return
        if (name === "result" && resultPane.blocks.hiddenWithin(match.begin, match.end))
            markersRevealed()
        editorNamed(name).select(match.begin, match.end)
        panesByRole.follow(name)
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

    // Showing the earlier version brings it to the result's position.
    function followEarlier(): void {
        panesByRole.paneShown(earlierPane.paneName)
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
        // The result first, then a source for a region the result does not map.
        const leads = ["result", leftPane.paneName, rightPane.paneName]
        for (let i = 0; i < leads.length; ++i) {
            const ranges = currentFor(leads[i], target, file)
            if (ranges.length > 0) {
                panesByRole.place(leads[i], ranges[0][0])
                return
            }
        }
        panesByRole.place("result", 1)
    }
    function followNavigation(): void {
        const target = controller.navigationTarget
        if (target.fileId === controller.selectedFileId && target.scope === "result")
            panesByRole.follow("result")
    }
    Connections {
        target: comparison.controller
        function onEditorPlaced(): void { Qt.callLater(comparison.placePanes) }
        function onNavigationChanged(): void { Qt.callLater(comparison.followNavigation) }
    }
    // Shows the markers when the cursor is on a folded line, as after going to a marker.
    function revealCursorLine(): void {
        if (resultPane.blocks.hiddenAt(resultPane.editor.cursorPosition))
            markersRevealed()
    }
    Component.onCompleted: {
        controller.setSyntaxBackground(schemeBackground)
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
    // The panes by role, scrolled together through the file's passages.
    ComparisonPanes {
        id: panesByRole
        left: leftPane
        result: resultPane
        right: rightPane
        earlier: earlierPane
        passages: comparison.file.passages || []
        lineSpacing: paneFont.lineSpacing
    }
    PassageSearch {
        id: passageSearch
        panes: panesByRole
        earlierShown: showEarlier.enabled && showEarlier.checked
    }

    ReadingBar {
        id: readingBar
        Layout.fillWidth: true
        search: passageSearch
        onFindRequested: comparison.findPassage()
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
            onScrolled: byUser => panesByRole.paneScrolled(leftPane.paneName, byUser)
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
            onScrolled: byUser => panesByRole.paneScrolled("result", byUser)
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
            onScrolled: byUser => panesByRole.paneScrolled(rightPane.paneName, byUser)
            onEditorCompleted: comparison.controller.attachSourceEditor(rightPane.editor)
        }
    }
    CheckBox {
        id: showEarlier
        objectName: "showEarlier"
        enabled: !!comparison.file.earlier && !!comparison.file.earlier.available
        text: enabled ? qsTr("Show common earlier version") : qsTr("Common earlier version unavailable")
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
        onScrolled: byUser => panesByRole.paneScrolled(earlierPane.paneName, byUser)
        onEditorCompleted: comparison.controller.attachSourceEditor(earlierPane.editor)
        onVisibleChanged: if (visible)
            Qt.callLater(comparison.followEarlier)
    }
}
