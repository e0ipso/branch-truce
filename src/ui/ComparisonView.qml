// The comparison of one file: the reading controls, the source and result panes with the
// connector bands between them, the region choices in the source panes' gutters, and the
// earlier version below.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: comparison
    required property ApplicationController controller
    required property SelectedFile file
    required property EditActions edits
    readonly property bool inputMethodComposing: resultPane.editor.inputMethodComposing
    property bool wrapLines: false
    property bool showWhitespace: false
    property bool showEarlier: false
    // Text panes never get shorter than this many lines; on a short window the page scrolls instead.
    property int minimumPaneLines: 8
    // Shows the raw lines of every conflict-marker block instead of folding them.
    property bool showMarkers: false
    // Asks for showMarkers, because something the user asked for is on a folded line.
    signal markersRevealed()
    // Shows the region choices in the source panes' gutters and the result's Take both menus.
    property bool showChoices: true
    // A choice of action for a source scope; the owner previews it when it needs confirming.
    signal choiceChosen(target: string, action: int)
    readonly property bool choosing: !!file.canEdit && !inputMethodComposing
    readonly property var markerFold: kept("fold", paneMarks.folds(file.markerBlocks, showMarkers))
    // The controller picks Breeze Light or Breeze Dark syntax colours from its lightness.
    readonly property color schemeBackground: Kirigami.Theme.backgroundColor

    // Pointer and focus are temporary decoration; navigation remains untouched.
    property string hoverOwner: ""
    property var hoverTarget: null
    property var focusTarget: null
    property string focusOwner: ""
    onFileChanged: {
        hoverOwner = ""
        hoverTarget = null
        focusOwner = ""
        focusTarget = null
    }
    function setFocus(id: string, active: bool): void {
        if (active) {
            focusOwner = id
            focusTarget = choiceTarget(id)
        } else if (focusOwner === id) {
            focusOwner = ""
            focusTarget = null
        }
    }
    readonly property var decorationTarget: hoverTarget || focusTarget || controller.navigationTarget
    function setHover(owner: string, target: var): void {
        if (target) {
            hoverOwner = owner
            hoverTarget = target
        } else if (hoverOwner === owner) {
            hoverOwner = ""
            hoverTarget = null
        }
    }
    function bandTarget(side: string, band: var): var {
        return {scope: "decoration", side: side, sourceBegin: band.sourceBegin,
            sourceEnd: band.sourceEnd, resultBegin: band.resultBegin, resultEnd: band.resultEnd}
    }
    function targetAt(side: string, line: int): var {
        const regions = file.regions || []
        for (let i = 0; i < regions.length; ++i) {
            const region = regions[i]
            if (side !== "result" && region.side !== side || side === "result" && !region.mapped)
                continue
            const begin = side === "result" ? region.resultLineBegin : region.sourceLineBegin
            const end = side === "result" ? region.resultLineEnd : region.sourceLineEnd
            if (line >= begin && line < Math.max(begin + 1, end))
                return choiceTarget(region.id)
        }
        const sides = side === "result" ? ["stage2", "stage3"] : [side]
        for (let i = 0; i < sides.length; ++i) {
            const bands = bandsFor(sides[i], file, markerFold)
            for (let j = 0; j < bands.length; ++j) {
                const band = bands[j]
                const begin = side === "result" ? band.resultBegin : band.sourceBegin
                const end = side === "result" ? band.resultEnd : band.sourceEnd
                if (line >= begin && line < Math.max(begin + 1, end))
                    return bandTarget(sides[i], band)
            }
        }
        return null
    }
    function choiceTarget(id: string): var {
        const regions = file.regions || []
        for (let i = 0; i < regions.length; ++i) {
            const region = regions[i]
            if (region.id === id)
                return {scope: "decoration", side: region.side, sourceBegin: region.sourceLineBegin,
                    sourceEnd: region.sourceLineEnd, resultBegin: region.mapped ? region.resultLineBegin : -1,
                    resultEnd: region.mapped ? region.resultLineEnd : -1}
        }
        return null
    }
    // The last value of each kept binding, with its JSON text.
    readonly property var keptValues: ({})
    // Derived marks, ranges, bands and folds retain their identity when their contents
    // stay equal, so the panes and line delegates only receive changed projections.
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
    function openSearch(): void { readingBar.open() }
    function updateSearch(): void {
        passageSearch.update(readingBar.query, readingBar.caseSensitive, readingBar.visible)
    }
    function findPassage(forward: bool): void {
        const name = passageSearch.scope
        const match = passageSearch.find(name, readingBar.query, forward)
        if (match.begin < 0)
            return
        if (name === "result" && resultPane.blocks.hiddenWithin(match.begin, match.end))
            markersRevealed()
        editorNamed(name).select(match.begin, match.end)
        updateSearch()
        panesByRole.follow(name)
    }
    PaneMarks { id: paneMarks }
    function paneLineMarks(pane: string): var {
        return paneMarks.marks(pane, file.passages, file.regions, file.handEditedLines,
                               file.markerBlocks, file.markerOrphans)
    }
    function navigationRanges(pane: string, target: var): var {
        return paneMarks.current(pane, target, controller.selectedFileId, file.passages, file.regions, resultPane.blocks)
    }
    // The connector bands between one source and the result: one per
    // non-equal passage of that side, in order, with its source and result
    // lines as [begin, end) ranges. The kind follows the passage: the result
    // changes the lines, adds a result-only passage's or drops a source-only
    // one's. The state of a region of that side on the source lines, or on
    // the boundary of an empty passage, outranks it as it does in the panes,
    // and a passage that meets a conflict-marker block of the result is a
    // conflict. The passages that meet a block the fold shows as one row, equal
    // ones inside it too, make one conflict band from all their source lines
    // to the whole block, which ends on that row.
    function bandsFor(side: string, file: SelectedFile, fold: var): var {
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
        const rows = Object.keys(fold.rows).map(Number)
        // The row of the folded block that the result lines [begin, end) meet, or 0.
        const rowMeeting = function(begin, end) {
            for (let i = 0; i < rows.length; ++i) {
                const stop = fold.rows[rows[i]].last + 1
                if (begin < end ? begin < stop && rows[i] < end : begin > rows[i] && begin < stop)
                    return rows[i]
            }
            return 0
        }
        const folded = {}
        const bands = []
        const passages = file.passages || []
        for (let i = 0; i < passages.length; ++i) {
            const passage = passages[i]
            if (passage.side !== side)
                continue
            const row = rowMeeting(passage.resultLineBegin, passage.resultLineEnd)
            const stop = row > 0 ? fold.rows[row].last + 1 : 0
            if (row > 0 && (passage.kind !== "equal"
                    || passage.resultLineBegin > row && passage.resultLineEnd <= stop)) {
                const band = folded[row]
                if (band) {
                    band.sourceBegin = Math.min(band.sourceBegin, passage.sourceLineBegin)
                    band.sourceEnd = Math.max(band.sourceEnd, passage.sourceLineEnd)
                    band.resultBegin = Math.min(band.resultBegin, passage.resultLineBegin)
                    band.resultEnd = Math.max(band.resultEnd, passage.resultLineEnd)
                } else {
                    folded[row] = {kind: "conflict", sourceBegin: passage.sourceLineBegin,
                        sourceEnd: passage.sourceLineEnd, resultBegin: Math.min(row, passage.resultLineBegin),
                        resultEnd: Math.max(stop, passage.resultLineEnd)}
                    bands.push(folded[row])
                }
                continue
            }
            if (passage.kind === "equal")
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
    // The choice rows of one source pane, in line order: one per line where a scope starts.
    // A section owns the row on its first line. A line choice owns the row on its line unless
    // a section starts there; then it rides in that row's extras, as does any other scope
    // that starts on a taken line. Each choice gains its region's undecided state.
    function choiceRowsFor(side: string, file: SelectedFile): var {
        const choices = file.choices || []
        const regions = file.regions || []
        const scoped = []
        for (let i = 0; i < choices.length && i < regions.length; ++i) {
            const region = regions[i]
            if (region.side !== side || region.id !== choices[i].id)
                continue
            scoped.push({line: region.sourceLineBegin, section: region.scope === "section",
                choice: Object.assign({undecided: region.state === "undecided"}, choices[i])})
        }
        const byLine = {}
        for (const sections of [true, false]) {
            for (let i = 0; i < scoped.length; ++i) {
                const entry = scoped[i]
                if (entry.section !== sections)
                    continue
                if (byLine[entry.line])
                    byLine[entry.line].extras.push(entry.choice)
                else
                    byLine[entry.line] = {line: entry.line, choice: entry.choice, extras: []}
            }
        }
        return Object.keys(byLine).map(Number).sort((a, b) => a - b).map(line => byLine[line])
    }
    // The result's section actions. decisions maps the first result line of each section
    // with a side mapped there to its state once no side is undecided, for the gutter; a
    // section in a folded block puts it on the block's row. combinations maps the row of a
    // folded block to a Take both offer for each section in it that is undecided and whose
    // sides can be combined.
    function resultSectionsFor(file: SelectedFile, fold: var): var {
        const rows = Object.keys(fold.rows).map(Number)
        const rowOf = function(line) {
            for (let i = 0; i < rows.length; ++i) {
                if (line >= rows[i] && line <= fold.rows[rows[i]].last)
                    return rows[i]
            }
            return 0
        }
        const choices = file.choices || []
        const regions = file.regions || []
        const sections = {}
        const order = []
        for (let i = 0; i < choices.length && i < regions.length; ++i) {
            const region = regions[i]
            if (region.scope !== "section" || region.id !== choices[i].id)
                continue
            if (!sections[region.section]) {
                sections[region.section] = {sides: {}, first: -1}
                order.push(region.section)
            }
            const section = sections[region.section]
            section.sides[region.side] = {region: region, choice: choices[i]}
            if (region.mapped && region.resultLineBegin > 0
                    && (section.first < 0 || region.resultLineBegin < section.first))
                section.first = region.resultLineBegin
        }
        const sideNames = {stage2: qsTr("Left"), stage3: qsTr("Right")}
        const stateNames = {accepted: qsTr("accepted"), ignored: qsTr("ignored"),
            manual: qsTr("edited by hand"), undecided: qsTr("undecided")}
        const decisions = {}
        const combinations = {}
        for (let i = 0; i < order.length; ++i) {
            const section = sections[order[i]]
            if (section.first < 0)
                continue
            const row = rowOf(section.first)
            const line = row > 0 ? row : section.first
            const sides = ["stage2", "stage3"].filter(side => !!section.sides[side])
            const states = sides.map(side => section.sides[side].region.state)
            if (states.indexOf("undecided") < 0) {
                if (decisions[line])
                    continue
                decisions[line] = {
                    state: states.indexOf("manual") >= 0 ? "manual" : states.indexOf("accepted") >= 0 ? "accepted" : "ignored",
                    label: sides.length === 2
                        ? qsTr("%1 %2, %3 %4").arg(sideNames[sides[0]])
                            .arg(stateNames[section.sides[sides[0]].region.state] || "")
                            .arg(sideNames[sides[1]]).arg(stateNames[section.sides[sides[1]].region.state] || "")
                        : qsTr("%1 %2").arg(sideNames[sides[0]])
                            .arg(stateNames[section.sides[sides[0]].region.state] || "")
                }
            } else if (row > 0 && sides.length === 2 && section.sides.stage2.choice.combinationAvailable) {
                if (!combinations[row])
                    combinations[row] = []
                combinations[row].push({id: section.sides.stage2.choice.id,
                    label: sides.map(side => section.sides[side].choice.label).join(qsTr(" and "))})
            }
        }
        return {decisions: decisions, combinations: combinations}
    }
    readonly property var resultSections: kept("sections:result", resultSectionsFor(file, markerFold))
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
            const ranges = navigationRanges(leads[i], target)
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
    // One text row; a gutter button is this high and wide.
    readonly property real rowHeight: Math.ceil(paneFont.lineSpacing)
    // A source pane's choice column: three buttons with a small spacing between and around them.
    readonly property real choiceWidth: 3 * rowHeight + 4 * Kirigami.Units.smallSpacing
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
        earlierShown: comparison.showEarlier && !!comparison.file.earlier && !!comparison.file.earlier.available
        onScopeIndexChanged: comparison.updateSearch()
    }

    Shortcut {
        sequence: StandardKey.Find
        onActivated: comparison.openSearch()
    }
    Shortcut {
        sequence: "Esc"
        enabled: readingBar.visible
        onActivated: readingBar.visible = false
    }
    Connections {
        target: comparison.editorNamed(passageSearch.scope) as TextEdit
        function onTextChanged(): void { comparison.updateSearch() }
        function onSelectionStartChanged(): void { comparison.updateSearch() }
        function onSelectionEndChanged(): void { comparison.updateSearch() }
    }
    ReadingBar {
        id: readingBar
        Layout.fillWidth: true
        search: passageSearch
        visible: false
        onChanged: comparison.updateSearch()
        onFindRequested: forward => comparison.findPassage(forward)
    }
    // The panes, with each source pane's region choices laid over the column it keeps for
    // them. The choices take no height, so showing them never shortens the panes.
    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true
        // A SplitView does not pass its panes' minimum height on to the layout.
        Layout.minimumHeight: Math.max(leftPane.Layout.minimumHeight, resultPane.Layout.minimumHeight,
            rightPane.Layout.minimumHeight)
        SplitView {
            id: panes
            objectName: "comparisonPanes"
            anchors.fill: parent
            orientation: Qt.Horizontal
            handle: ConnectorHandle {
                id: connectorHandle
                result: resultPane
                source: connectorHandle.leftSide ? leftPane : rightPane
                bands: comparison.kept("bands:" + connectorHandle.source.paneName,
                    comparison.bandsFor(connectorHandle.source.paneName, comparison.file, comparison.markerFold))
                current: comparison.kept("current:band:" + connectorHandle.source.paneName,
                    comparison.navigationRanges(connectorHandle.source.paneName, comparison.decorationTarget))
                onBandHovered: band => comparison.setHover(connectorHandle.source.paneName + "Band",
                    band ? comparison.bandTarget(connectorHandle.source.paneName, band) : null)
            }
            SourcePane {
                id: leftPane
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                SplitView.preferredWidth: panes.width / 3
                paneName: "stage2"
                paneRole: qsTr("Left source")
                choiceEdge: "right"
                choiceWidth: comparison.showChoices ? comparison.choiceWidth : 0
                source: comparison.file.stage2 || ({})
                marks: comparison.kept("marks:stage2", comparison.paneLineMarks(leftPane.paneName))
                current: comparison.kept("current:stage2", comparison.navigationRanges(leftPane.paneName, comparison.decorationTarget))
                tabStop: comparison.tabStop
                wrapLines: comparison.wrapLines
                showWhitespace: comparison.showWhitespace
                minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
                onLineHovered: line => comparison.setHover("stage2", line > 0 ? comparison.targetAt("stage2", line) : null)
                onScrolled: byUser => panesByRole.paneScrolled(leftPane.paneName, byUser)
                onEditorCompleted: comparison.controller.attachSourceEditor(leftPane.editor)
            }
            ResultPane {
                id: resultPane
                edits: comparison.edits
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 10
                SplitView.preferredWidth: panes.width / 3
                controller: comparison.controller
                file: comparison.file
                marks: comparison.kept("marks:result", comparison.paneLineMarks("result"))
                current: comparison.kept("current:result", comparison.navigationRanges("result", comparison.decorationTarget))
                fold: comparison.markerFold
                decisions: comparison.resultSections.decisions
                combinations: comparison.showChoices ? comparison.resultSections.combinations : ({})
                choosing: comparison.choosing
                onChoiceChosen: (target, action) => comparison.choiceChosen(target, action)
                onChoiceFocused: (target, active) => comparison.setFocus(target, active)
                tabStop: comparison.tabStop
                wrapLines: comparison.wrapLines
                showWhitespace: comparison.showWhitespace
                minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
                onLineHovered: line => comparison.setHover("result", line > 0 ? comparison.targetAt("result", line) : null)
                onScrolled: byUser => panesByRole.paneScrolled("result", byUser)
            }
            SourcePane {
                id: rightPane
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                SplitView.preferredWidth: panes.width / 3
                paneName: "stage3"
                paneRole: qsTr("Right source")
                choiceEdge: "left"
                choiceWidth: comparison.showChoices ? comparison.choiceWidth : 0
                source: comparison.file.stage3 || ({})
                marks: comparison.kept("marks:stage3", comparison.paneLineMarks(rightPane.paneName))
                current: comparison.kept("current:stage3", comparison.navigationRanges(rightPane.paneName, comparison.decorationTarget))
                tabStop: comparison.tabStop
                wrapLines: comparison.wrapLines
                showWhitespace: comparison.showWhitespace
                minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
                onLineHovered: line => comparison.setHover("stage3", line > 0 ? comparison.targetAt("stage3", line) : null)
                onScrolled: byUser => panesByRole.paneScrolled(rightPane.paneName, byUser)
                onEditorCompleted: comparison.controller.attachSourceEditor(rightPane.editor)
            }
        }
        Item {
            id: scopedChoices
            objectName: "scopedChoices"
            anchors.fill: parent
            visible: comparison.showChoices
            ChoiceLane {
                pane: leftPane
                rows: comparison.kept("choices:stage2", comparison.choiceRowsFor(leftPane.paneName, comparison.file))
                rowHeight: comparison.rowHeight
                choosing: comparison.choosing
                onHovered: target => comparison.setHover("stage2Choice", target ? comparison.choiceTarget(target) : null)
                onFocused: (target, active) => comparison.setFocus(target, active)
                onChosen: (target, action) => comparison.choiceChosen(target, action)
            }
            ChoiceLane {
                pane: rightPane
                rows: comparison.kept("choices:stage3", comparison.choiceRowsFor(rightPane.paneName, comparison.file))
                rowHeight: comparison.rowHeight
                choosing: comparison.choosing
                onHovered: target => comparison.setHover("stage3Choice", target ? comparison.choiceTarget(target) : null)
                onFocused: (target, active) => comparison.setFocus(target, active)
                onChosen: (target, action) => comparison.choiceChosen(target, action)
            }
        }
    }
    SourcePane {
        id: earlierPane
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(comparison.height / 3, Kirigami.Units.gridUnit * 10)
        visible: comparison.showEarlier && !!comparison.file.earlier && !!comparison.file.earlier.available
        paneName: "earlier"
        paneRole: qsTr("Earlier-version context · read-only")
        source: comparison.file.earlier || ({})
        marks: comparison.kept("marks:earlier", comparison.paneLineMarks(earlierPane.paneName))
        current: comparison.kept("current:earlier", comparison.navigationRanges(earlierPane.paneName, comparison.decorationTarget))
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
