// The comparison of one file: the reading controls, the source and result panes with the
// connector bands between them, hover choices over the source panes, and the
// earlier version below.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: comparison
    objectName: "comparisonView"
    spacing: Kirigami.Units.smallSpacing
    required property ResultEditorAdapter editorAdapter
    required property FileNavigator navigator
    required property SessionLoader loader
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
    // Enables the source hover choices and the result's Take both menus.
    property bool showChoices: true
    // A choice of action for a source scope; the owner previews it when it needs confirming.
    signal choiceChosen(target: string, action: int)
    readonly property bool choosing: !!file.canEdit && !inputMethodComposing
    readonly property var markerFold: kept("fold", paneMarks.folds(file.markerBlocks, showMarkers))
    // The editor adapter picks Breeze Light or Breeze Dark syntax colours from its lightness.
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
    readonly property var decorationTarget: hoverTarget || focusTarget || navigator.target
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
            const projection = sides[i] === "stage2" ? leftBands : rightBands
            const band = projection.bandForLine(line, side === "result")
            if (band.kind)
                return bandTarget(sides[i], band)
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
        return paneMarks.current(pane, target, file.fileId, file.passages, file.regions, resultPane.blocks)
    }
    // Each projection follows the two text layouts and their scrolling positions.
    component BandProjection: ConnectorBands {
        required property SourcePane pane
        side: pane.paneName
        passages: comparison.file.passages || []
        regions: comparison.file.regions || []
        markerBlocks: comparison.file.markerBlocks || []
        fold: comparison.markerFold
        current: comparison.kept("current:band:" + pane.paneName,
            comparison.navigationRanges(pane.paneName, comparison.decorationTarget))
        sourceBlocks: pane.blocks
        resultBlocks: resultPane.blocks
        sourceParentY: pane.viewport.parent.y
        resultParentY: resultPane.viewport.parent.y
        sourceEditorY: pane.editor.y
        resultEditorY: resultPane.editor.y
        sourcePadding: pane.editor.topPadding
        resultPadding: resultPane.editor.topPadding
        sourceScroll: pane.viewport.contentY
        resultScroll: resultPane.viewport.contentY
        viewportHeight: pane.viewport.height
        rule: Kirigami.Units.smallSpacing / 2
    }
    BandProjection { id: leftBands; pane: leftPane }
    BandProjection { id: rightBands; pane: rightPane }
    // One primary row at each actual source range's start. The section owns
    // that row; all its precise line scopes remain commands in its menu.
    function choiceRowsFor(side: string, file: SelectedFile): var {
        const choices = file.choices || []
        const regions = file.regions || []
        const scoped = []
        for (let i = 0; i < choices.length && i < regions.length; ++i) {
            const region = regions[i]
            if (region.side !== side || region.id !== choices[i].id)
                continue
            scoped.push({line: region.sourceLineBegin, end: region.sourceLineEnd,
                section: region.section, whole: region.scope === "section",
                choice: Object.assign({undecided: region.state === "undecided"}, choices[i])})
        }
        const rows = scoped.filter(entry => entry.whole).map(entry => ({
            line: entry.line, end: entry.end, section: entry.section,
            choice: entry.choice, extras: []
        }))
        for (let i = 0; i < scoped.length; ++i) {
            const entry = scoped[i]
            if (entry.whole)
                continue
            const owner = rows.find(row => row.section === entry.section
                && row.line <= entry.line && entry.end <= row.end)
            if (owner)
                owner.extras.push(entry.choice)
            else
                // Retain a standalone scope if no section owns its actual range.
                rows.push({line: entry.line, end: entry.end, section: entry.section,
                    choice: entry.choice, extras: []})
        }
        return rows.sort((a, b) => a.line - b.line)
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
        // One whole phrase per side and state, so translators can order each.
        const sideStates = {
            stage2: {accepted: qsTr("Left accepted"), ignored: qsTr("Left ignored"),
                manual: qsTr("Left edited by hand"), undecided: qsTr("Left undecided")},
            stage3: {accepted: qsTr("Right accepted"), ignored: qsTr("Right ignored"),
                manual: qsTr("Right edited by hand"), undecided: qsTr("Right undecided")}}
        const decisions = {}
        const combinations = {}
        for (let i = 0; i < order.length; ++i) {
            const section = sections[order[i]]
            const sideState = side => sideStates[side][section.sides[side].region.state] || ""
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
                        ? qsTr("%1, %2").arg(sideState(sides[0])).arg(sideState(sides[1]))
                        : sideState(sides[0])
                }
            } else if (row > 0 && sides.length === 2 && section.sides.stage2.choice.combinationAvailable
                       && (fold.rows[row].sections || []).indexOf(order[i]) >= 0) {
                if (!combinations[row])
                    combinations[row] = []
                combinations[row].push({id: section.sides.stage2.choice.id,
                    label: qsTr("%1 and %2").arg(section.sides.stage2.choice.label).arg(section.sides.stage3.choice.label)})
            }
        }
        return {decisions: decisions, combinations: combinations}
    }
    readonly property var resultSections: kept("sections:result", resultSectionsFor(file, markerFold))
    // The conflict the source headers resolve: its section and each side's section choice,
    // or an empty object when no undecided conflict is left. Folded marker blocks move
    // the cursor past them, so the stepper's stop and the next conflict also count.
    readonly property var activeConflict: kept("conflict:active", activeConflictFor(file,
        resultPane.blocks.lineAt(resultPane.editor.cursorPosition), navigator.target))
    // The undecided conflict at the cursor's line, its marker lines included; else the
    // stepper's undecided stop in this file; else the next undecided conflict after the
    // cursor, wrapping to the first.
    function activeConflictFor(file: SelectedFile, line: int, navigation: var): var {
        const choices = file.choices || []
        const regions = file.regions || []
        const sections = {}
        const owners = {}
        for (let i = 0; i < choices.length && i < regions.length; ++i) {
            const region = regions[i]
            if (region.id !== choices[i].id)
                continue
            owners[region.id] = region.section
            if (region.scope !== "section")
                continue
            if (!sections[region.section])
                sections[region.section] = {begin: -1, end: -1, marked: false}
            const section = sections[region.section]
            section[region.side] = Object.assign({undecided: region.state === "undecided"}, choices[i])
            if (region.mapped && region.resultLineBegin > 0) {
                const end = Math.max(region.resultLineBegin + 1, region.resultLineEnd)
                section.begin = section.begin < 0 ? region.resultLineBegin : Math.min(section.begin, region.resultLineBegin)
                section.end = Math.max(section.end, end)
            }
        }
        // A marker block spans its conflict's marker lines too.
        const blocks = file.markerBlocks || []
        for (let i = 0; i < blocks.length; ++i) {
            const ids = blocks[i].sections || []
            for (let j = 0; j < ids.length; ++j) {
                const section = sections[ids[j]]
                if (!section)
                    continue
                section.marked = true
                section.begin = blocks[i].resultLineBegin
                section.end = blocks[i].resultLineEnd
            }
        }
        // A conflict is a marked section or one with a contribution from each side.
        const conflicts = Object.keys(sections).map(id => Object.assign({section: id}, sections[id]))
            .filter(entry => entry.marked || !!entry.stage2 && !!entry.stage3)
            .filter(entry => [entry.stage2, entry.stage3].some(choice => !!choice && choice.undecided))
            .sort((a, b) => a.begin - b.begin)
        const view = entry => entry ? {section: entry.section, stage2: entry.stage2 || null,
            stage3: entry.stage3 || null} : {}
        const here = conflicts.find(entry => entry.begin > 0 && line >= entry.begin && line < entry.end)
        if (here)
            return view(here)
        if (navigation && navigation.fileId === file.fileId && navigation.contributionId) {
            const stop = conflicts.find(entry => entry.section === owners[navigation.contributionId])
            if (stop)
                return view(stop)
        }
        return view(conflicts.find(entry => entry.begin >= line) || conflicts[0])
    }
    function resolvable(side: string): bool {
        const choice = activeConflict[side]
        return !!choice && !!choice.available
    }
    function resolveText(side: string): string {
        return side === "stage2" ? qsTr("Resolve the current conflict using Left")
            : qsTr("Resolve the current conflict using Right")
    }
    // The conflict the header would resolve, or why it cannot.
    function resolveHint(side: string): string {
        if (!activeConflict.section)
            return qsTr("No undecided conflict is left in this file.")
        const choice = activeConflict[side]
        if (!choice)
            return qsTr("This side has no contribution to the conflict.")
        if (!choice.available)
            return choice.detail || qsTr("This side of the conflict is already decided.")
        return choice.label
    }
    function followEarlier(): void {
        panesByRole.paneShown(earlierPane.paneName)
    }
    // A newly selected file opens with the region the editor put its cursor on
    // at the reference row of every pane, or at the top without one. A
    // navigation in the file since then keeps its own position.
    function placePanes(): void {
        const target = editorAdapter.placement
        if (!target || target.fileId !== file.fileId)
            return
        const navigation = navigator.target
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
        const target = navigator.target
        if (target.fileId === file.fileId && target.scope === "result") {
            resultPane.editor.forceActiveFocus(Qt.ShortcutFocusReason)
            panesByRole.follow("result")
        }
    }
    Connections {
        target: comparison.editorAdapter
        function onPlaced(): void { Qt.callLater(comparison.placePanes) }
    }
    Connections {
        target: comparison.navigator
        function onChanged(): void { Qt.callLater(comparison.followNavigation) }
    }
    // Shows the markers when the cursor is on a folded line, as after going to a marker.
    function revealCursorLine(): void {
        if (resultPane.blocks.hiddenAt(resultPane.editor.cursorPosition))
            markersRevealed()
    }
    Component.onCompleted: {
        editorAdapter.setSyntaxBackground(schemeBackground)
        Qt.callLater(placePanes)
    }
    onSchemeBackgroundChanged: editorAdapter.setSyntaxBackground(schemeBackground)

    function paneMinimumHeight(lines: int): real {
        return Math.ceil(lines * rowHeight + resultPane.editor.topPadding
            + resultPane.editor.bottomPadding)
    }
    FontMetrics {
        id: paneFont
        font: resultPane.editor.font
    }
    TextMetrics {
        id: tabMetrics
        font: resultPane.editor.font
        text: "    "
    }
    // Measure actual spaces and update when the desktop's fixed-width font changes.
    // Some font files report a different average width from their glyph advances.
    readonly property real tabStop: tabMetrics.advanceWidth
    // The same laid-out advance used by gutters, bands and scroll mapping.
    // CodeRows owns the line distance; no consumer adds a second gap.
    readonly property real rowHeight: Math.ceil(resultPane.blocks.rowHeight)
    // The panes by role, scrolled together through the file's passages.
    ComparisonPanes {
        id: panesByRole
        left: leftPane
        result: resultPane
        right: rightPane
        earlier: earlierPane
        passages: comparison.file.passages || []
        lineSpacing: comparison.rowHeight
    }
    PassageSearch {
        id: passageSearch
        panes: panesByRole
        earlierShown: comparison.showEarlier && !!comparison.file.earlier && !!comparison.file.earlier.available
        matchColor: Qt.alpha(Kirigami.Theme.neutralBackgroundColor, 0.75)
        onScopeIndexChanged: comparison.updateSearch()
    }

    Shortcut {
        sequences: [StandardKey.Find]
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
            // Each handle includes its two overlaid hairlines. Reserve both
            // handles once before sharing the usable width among the panes.
            readonly property real connectorWidth: Kirigami.Units.gridUnit
            readonly property real defaultPaneWidth: Math.max(0, width - 2 * connectorWidth) / 3
            handle: ConnectorHandle {
                id: connectorHandle
                implicitWidth: panes.connectorWidth
                result: resultPane
                source: connectorHandle.leftSide ? leftPane : rightPane
                projection: connectorHandle.leftSide ? leftBands : rightBands
                onBandHovered: band => comparison.setHover(connectorHandle.source.paneName + "Band",
                    band ? comparison.bandTarget(connectorHandle.source.paneName, band) : null)
            }
            SourcePane {
                id: leftPane
                edits: comparison.edits
                function openChoicesAt(line: int, x: real, y: real): bool {
                    return leftChoices.openMenuForLine(line, x, y)
                }
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                SplitView.preferredWidth: panes.defaultPaneWidth
                paneName: "stage2"
                paneRole: qsTr("Left source")
                choiceEdge: "right"
                resolveText: comparison.resolveText("stage2")
                resolveHint: comparison.resolveHint("stage2")
                resolveEnabled: comparison.choosing && comparison.resolvable("stage2")
                onResolveChosen: {
                    const id = comparison.activeConflict.stage2.id
                    comparison.setHover("stage2Resolve", null)
                    comparison.choiceChosen(id, ChoicePreviewFlow.ResolveUsingSide)
                }
                onResolveHovered: hovered => comparison.setHover("stage2Resolve", hovered && comparison.resolvable("stage2")
                    ? comparison.choiceTarget(comparison.activeConflict.stage2.id) : null)
                source: comparison.file.stage2 || ({})
                versionName: source.commitId === comparison.loader.stage2CommitId
                    ? comparison.loader.stage2Name : ""
                marks: comparison.kept("marks:stage2", comparison.paneLineMarks(leftPane.paneName))
                current: comparison.kept("current:stage2", comparison.navigationRanges(leftPane.paneName, comparison.decorationTarget))
                tabStop: comparison.tabStop
                wrapLines: comparison.wrapLines
                showWhitespace: comparison.showWhitespace
                minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
                onLineHovered: line => comparison.setHover("stage2", line > 0 ? comparison.targetAt("stage2", line) : null)
                onScrolled: byUser => panesByRole.paneScrolled(leftPane.paneName, byUser)
                onEditorCompleted: comparison.editorAdapter.attachSource(leftPane.editor)
            }
            ResultPane {
                id: resultPane
                edits: comparison.edits
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 10
                SplitView.preferredWidth: panes.defaultPaneWidth
                editorAdapter: comparison.editorAdapter
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
                edits: comparison.edits
                function openChoicesAt(line: int, x: real, y: real): bool {
                    return rightChoices.openMenuForLine(line, x, y)
                }
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                SplitView.preferredWidth: panes.defaultPaneWidth
                paneName: "stage3"
                paneRole: qsTr("Right source")
                choiceEdge: "left"
                resolveText: comparison.resolveText("stage3")
                resolveHint: comparison.resolveHint("stage3")
                resolveEnabled: comparison.choosing && comparison.resolvable("stage3")
                onResolveChosen: {
                    const id = comparison.activeConflict.stage3.id
                    comparison.setHover("stage3Resolve", null)
                    comparison.choiceChosen(id, ChoicePreviewFlow.ResolveUsingSide)
                }
                onResolveHovered: hovered => comparison.setHover("stage3Resolve", hovered && comparison.resolvable("stage3")
                    ? comparison.choiceTarget(comparison.activeConflict.stage3.id) : null)
                source: comparison.file.stage3 || ({})
                versionName: source.commitId === comparison.loader.stage3CommitId
                    ? comparison.loader.stage3Name : ""
                marks: comparison.kept("marks:stage3", comparison.paneLineMarks(rightPane.paneName))
                current: comparison.kept("current:stage3", comparison.navigationRanges(rightPane.paneName, comparison.decorationTarget))
                tabStop: comparison.tabStop
                wrapLines: comparison.wrapLines
                showWhitespace: comparison.showWhitespace
                minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
                onLineHovered: line => comparison.setHover("stage3", line > 0 ? comparison.targetAt("stage3", line) : null)
                onScrolled: byUser => panesByRole.paneScrolled(rightPane.paneName, byUser)
                onEditorCompleted: comparison.editorAdapter.attachSource(rightPane.editor)
            }
        }
        Item {
            id: scopedChoices
            objectName: "scopedChoices"
            anchors.fill: parent
            visible: comparison.showChoices
            ChoiceLane {
                id: leftChoices
                pane: leftPane
                rows: comparison.kept("choices:stage2", comparison.choiceRowsFor(leftPane.paneName, comparison.file))
                rowHeight: comparison.rowHeight
                choosing: comparison.choosing
                onHovered: target => comparison.setHover("stage2Choice", target ? comparison.choiceTarget(target) : null)
                onFocused: (target, active) => comparison.setFocus(target, active)
                onChosen: (target, action) => comparison.choiceChosen(target, action)
            }
            ChoiceLane {
                id: rightChoices
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
        edits: comparison.edits
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
        onEditorCompleted: comparison.editorAdapter.attachSource(earlierPane.editor)
        onVisibleChanged: if (visible)
            Qt.callLater(comparison.followEarlier)
    }
}
