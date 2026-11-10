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
    readonly property ComparisonPresentation presentation: file.comparison
    readonly property var markerFold: paneMarks.fold
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
            focusTarget = presentation.choiceTarget(id)
        } else if (focusOwner === id) {
            focusOwner = ""
            focusTarget = null
        }
    }
    readonly property var decorationTarget: {
        const generation = presentation.generation
        if (hoverTarget && hoverTarget.generation === generation) return hoverTarget
        if (focusTarget && focusTarget.generation === generation) return focusTarget
        return navigator.target
    }
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
        return {scope: "decoration", fileId: presentation.fileId, revision: presentation.revision,
            correspondence: presentation.correspondence, generation: presentation.generation,
            side: side, sourceBegin: band.sourceBegin,
            sourceEnd: band.sourceEnd, resultBegin: band.resultBegin, resultEnd: band.resultEnd}
    }
    function targetAt(side: string, line: int): var {
        const target = presentation.targetAt(side, line)
        if (target.scope) return target
        const sides = side === "result" ? ["stage2", "stage3"] : [side]
        for (let i = 0; i < sides.length; ++i) {
            const projection = sides[i] === "stage2" ? leftBands : rightBands
            const band = projection.bandForLine(line, side === "result")
            if (band.kind)
                return bandTarget(sides[i], band)
        }
        return null
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
    PaneMarks { id: paneMarks; presentation: comparison.presentation; markersShown: comparison.showMarkers }
    function navigationRanges(pane: string, target: var): var {
        return paneMarks.current(pane, target, file.fileId, presentation.passages, presentation.contributions, resultPane.blocks)
    }
    // Each projection follows the two text layouts and their scrolling positions.
    component BandProjection: ConnectorBands {
        required property SourcePane pane
        side: pane.paneName
        passages: comparison.presentation.passages
        contributions: comparison.presentation.contributions
        markerBlocks: comparison.presentation.markerBlocks
        fold: comparison.markerFold
        current: comparison.navigationRanges(pane.paneName, comparison.decorationTarget)
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
    readonly property var resultSections: paneMarks.sections
    readonly property var activeConflict: presentation.activeConflict(
        resultPane.blocks.lineAt(resultPane.editor.cursorPosition), navigator.target, presentation.generation)
    function resolvable(side: string): bool {
        const choice = activeConflict[side]
        return !!choice && !!choice.id && !!choice.available
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
        if (!choice || !choice.id)
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
        if (!target || !presentation.accepts(target))
            return
        const navigation = navigator.target
        if (presentation.accepts(navigation) && navigation.fileId === target.fileId
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
        if (presentation.accepts(target) && target.scope === "result") {
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
        passages: comparison.presentation.passages
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
    // Both captured sources share layout, reading controls and scope routing.
    component ComparedSource: SourcePane {
        id: sourcePane
        required property ChoiceLane choices
        readonly property bool leftSide: paneName === "stage2"
        edits: comparison.edits
        function openChoicesAt(line: int, x: real, y: real): bool {
            return choices.openMenuForLine(line, x, y)
        }
        SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
        SplitView.preferredWidth: panes.defaultPaneWidth
        choiceEdge: leftSide ? "right" : "left"
        resolveText: comparison.resolveText(paneName)
        resolveHint: comparison.resolveHint(paneName)
        resolveEnabled: comparison.choosing && comparison.resolvable(paneName)
        onResolveChosen: {
            const id = comparison.activeConflict[paneName].id
            comparison.setHover(paneName + "Resolve", null)
            comparison.choiceChosen(id, ChoicePreviewFlow.ResolveUsingSide)
        }
        onResolveHovered: hovered => comparison.setHover(paneName + "Resolve", hovered && comparison.resolvable(paneName)
            ? comparison.presentation.choiceTarget(comparison.activeConflict[paneName].id) : null)
        source: leftSide ? comparison.file.stage2 : comparison.file.stage3
        versionName: source.commitId === (leftSide ? comparison.loader.stage2CommitId : comparison.loader.stage3CommitId)
            ? (leftSide ? comparison.loader.stage2Name : comparison.loader.stage3Name) : ""
        marks: paneMarks.lineMarks[paneName]
        current: comparison.navigationRanges(paneName, comparison.decorationTarget)
        tabStop: comparison.tabStop
        wrapLines: comparison.wrapLines
        showWhitespace: comparison.showWhitespace
        minimumTextHeight: comparison.paneMinimumHeight(comparison.minimumPaneLines)
        onLineHovered: line => comparison.setHover(paneName, line > 0 ? comparison.targetAt(paneName, line) : null)
        onScrolled: byUser => panesByRole.paneScrolled(paneName, byUser)
        onEditorCompleted: comparison.editorAdapter.attachSource(sourcePane.editor)
    }
    // Source choices overlay the viewports without reserving pane space.
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
            ComparedSource { id: leftPane; paneName: "stage2"; paneRole: qsTr("Left source"); choices: leftChoices }
            ResultPane {
                id: resultPane
                edits: comparison.edits
                SplitView.minimumWidth: Kirigami.Units.gridUnit * 10
                SplitView.preferredWidth: panes.defaultPaneWidth
                editorAdapter: comparison.editorAdapter
                marks: paneMarks.lineMarks.result
                current: comparison.navigationRanges("result", comparison.decorationTarget)
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
            ComparedSource { id: rightPane; paneName: "stage3"; paneRole: qsTr("Right source"); choices: rightChoices }
        }
        Item {
            id: scopedChoices
            objectName: "scopedChoices"
            anchors.fill: parent
            visible: comparison.showChoices
            ChoiceLane {
                id: leftChoices
                pane: leftPane
                rows: comparison.presentation.leftRows
                rowHeight: comparison.rowHeight
                choosing: comparison.choosing
                onHovered: target => comparison.setHover("stage2Choice", target ? comparison.presentation.choiceTarget(target) : null)
                onFocused: (target, active) => comparison.setFocus(target, active)
                onChosen: (target, action) => comparison.choiceChosen(target, action)
            }
            ChoiceLane {
                id: rightChoices
                pane: rightPane
                rows: comparison.presentation.rightRows
                rowHeight: comparison.rowHeight
                choosing: comparison.choosing
                onHovered: target => comparison.setHover("stage3Choice", target ? comparison.presentation.choiceTarget(target) : null)
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
        marks: paneMarks.lineMarks.earlier
        current: comparison.navigationRanges(earlierPane.paneName, comparison.decorationTarget)
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
