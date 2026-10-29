// A SplitView handle between a source pane and the result, holding that source's connector
// bands. A band joins a passage's lines on the source's edge to its lines on the result's
// edge, each at the line's y in its pane less the pane's scroll. An empty range ends in a
// thin wedge on its line boundary, where the pane draws its gap rule. The bands cover the
// panes' viewports and clip what they scroll out of view, and only those that cross it are
// made. They take no input or focus, so the split still drags the handle.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import org.kde.kirigami as Kirigami

Item {
    id: connector
    required property ResultPane result
    // A handle starts at x 0, before the split places it, and the right
    // source may not exist yet, so it counts as the left one until placed.
    readonly property bool leftSide: x <= result.x
    // leftSide picks the source: the left pane, or the right one once placed.
    required property SourcePane source
    // The source's connector bands, in order at both ends.
    required property var bands
    required property var current
    signal bandHovered(var band)
    function isCurrent(band: var): bool {
        for (let i = 0; i < current.length; ++i) {
            const range = current[i]
            if (band.sourceBegin < Math.max(range[1], range[0] + 1)
                    && range[0] < Math.max(band.sourceEnd, band.sourceBegin + 1))
                return true
        }
        return false
    }
    function bandAt(point: point): var {
        const y = point.y - source.viewport.parent.y
        const ratio = leftSide ? point.x / width : 1 - point.x / width
        for (let i = firstShown; i < endShown; ++i) {
            const band = bands[i]
            const top = edge(true, band.sourceBegin, band.sourceEnd, false, layoutRevision) * (1 - ratio)
                + edge(false, band.resultBegin, band.resultEnd, false, layoutRevision) * ratio
            const bottom = edge(true, band.sourceBegin, band.sourceEnd, true, layoutRevision) * (1 - ratio)
                + edge(false, band.resultBegin, band.resultEnd, true, layoutRevision) * ratio
            if (y >= top && y <= bottom)
                return band
        }
        return null
    }
    HoverHandler {
        onHoveredChanged: connector.bandHovered(hovered ? connector.bandAt(point.position) : null)
        onPointChanged: if (hovered) connector.bandHovered(connector.bandAt(point.position))
    }
    // Counts relayouts of either pane's text, so the band ends follow them.
    property int layoutRevision: 0
    readonly property real rule: Kirigami.Units.smallSpacing / 2
    readonly property int firstShown: shownIndex(false, layoutRevision)
    readonly property int endShown: shownIndex(true, layoutRevision)
    // The y in the bands of the top, or the bottom, of lines [begin, end)
    // of the source or of the result. revision is layoutRevision, so a
    // binding follows relayouts.
    function edge(inSource: bool, begin: int, end: int, bottom: bool, revision: int): real {
        const viewport = inSource ? source.viewport : result.viewport
        const editor = inSource ? source.editor : result.editor
        const blocks = inSource ? source.blocks : result.blocks
        const y = viewport.parent.y - source.viewport.parent.y + editor.y + editor.topPadding
            + blocks.yAtLine(bottom ? end : begin) - viewport.contentY
        return begin < end ? y : y + (bottom ? rule : -rule) / 2
    }
    // The index of the first band not wholly above the view, or with
    // below set, of the first band wholly below it. Both ends of the
    // bands are in order, so each of those is a run at one end.
    function shownIndex(below: bool, revision: int): int {
        const extent = below ? source.viewport.height : 0
        const sourceLine = source.blocks.lineAtY(source.viewport.contentY + extent
            - source.editor.y - source.editor.topPadding)
        const resultLine = result.blocks.lineAtY(result.viewport.contentY + extent
            - result.editor.y - result.editor.topPadding)
        let low = 0
        let high = bands.length
        while (low < high) {
            const middle = (low + high) >> 1
            const band = bands[middle]
            const found = below ? band.sourceBegin > sourceLine && band.resultBegin > resultLine
                : band.sourceEnd >= sourceLine || band.resultEnd >= resultLine
            if (found)
                high = middle
            else
                low = middle + 1
        }
        return low
    }
    objectName: source.paneName + "Connector"
    implicitWidth: Kirigami.Units.gridUnit
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    Accessible.ignored: true
    Connections {
        target: connector.source.blocks
        function onLayoutChanged(): void { ++connector.layoutRevision }
    }
    Connections {
        target: connector.result.blocks
        function onLayoutChanged(): void { ++connector.layoutRevision }
    }
    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.highlightColor
        opacity: connector.SplitHandle.pressed ? 0.3 : connector.SplitHandle.hovered ? 0.15 : 0
    }
    Item {
        id: bandLayer
        objectName: connector.source.paneName + "Bands"
        y: connector.source.viewport.parent.y
        width: connector.width
        height: connector.source.viewport.height
        clip: true
        enabled: false
        Repeater {
            model: connector.bands.slice(connector.firstShown, connector.endShown)
            delegate: Shape {
                id: band
                required property var modelData
                readonly property string kind: modelData.kind
                // Under the disabled layer Kirigami would give the band the
                // default Window set instead of inheriting the handle's View set.
                Kirigami.Theme.colorSet: Kirigami.Theme.View
                Kirigami.Theme.inherit: false
                readonly property real sourceTop: connector.edge(true, modelData.sourceBegin,
                    modelData.sourceEnd, false, connector.layoutRevision)
                readonly property real sourceBottom: connector.edge(true, modelData.sourceBegin,
                    modelData.sourceEnd, true, connector.layoutRevision)
                readonly property real resultTop: connector.edge(false, modelData.resultBegin,
                    modelData.resultEnd, false, connector.layoutRevision)
                readonly property real resultBottom: connector.edge(false, modelData.resultBegin,
                    modelData.resultEnd, true, connector.layoutRevision)
                readonly property real sourceX: connector.leftSide ? 0 : width
                readonly property real resultX: width - sourceX
                // The passage's background role; removed and ignored lines get a faint tint.
                readonly property bool current: connector.isCurrent(modelData)
                readonly property color fill: current ? Kirigami.Theme.highlightColor : kind === "added" || kind === "accepted"
                    ? Kirigami.Theme.positiveBackgroundColor
                    : kind === "changed" ? Kirigami.Theme.activeBackgroundColor
                    : kind === "conflict" ? Kirigami.Theme.negativeBackgroundColor
                    : kind === "manual" ? Kirigami.Theme.neutralBackgroundColor
                    : Kirigami.Theme.disabledTextColor
                width: bandLayer.width
                height: bandLayer.height
                opacity: current ? 0.3 : kind === "removed" || kind === "ignored" ? 0.15 : 1
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeWidth: -1
                    fillColor: band.fill
                    startX: band.sourceX
                    startY: band.sourceTop
                    PathLine { x: band.resultX; y: band.resultTop }
                    PathLine { x: band.resultX; y: band.resultBottom }
                    PathLine { x: band.sourceX; y: band.sourceBottom }
                    PathLine { x: band.sourceX; y: band.sourceTop }
                }
            }
        }
    }
}
