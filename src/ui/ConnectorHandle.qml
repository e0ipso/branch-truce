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
    required property ConnectorBands projection
    signal bandHovered(var band)
    function bandAt(point: point): var {
        const band = projection.bandAt(point.x, point.y, width, leftSide)
        return band.kind ? band : null
    }
    HoverHandler {
        onHoveredChanged: connector.bandHovered(hovered ? connector.bandAt(point.position) : null)
        onPointChanged: if (hovered) connector.bandHovered(connector.bandAt(point.position))
    }
    objectName: source.paneName + "Connector"
    implicitWidth: Kirigami.Units.gridUnit
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    Accessible.ignored: true
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
            model: connector.projection.visibleBands
            delegate: Shape {
                id: band
                required property var modelData
                readonly property string kind: modelData.kind
                // Under the disabled layer Kirigami would give the band the
                // default Window set instead of inheriting the handle's View set.
                Kirigami.Theme.colorSet: Kirigami.Theme.View
                Kirigami.Theme.inherit: false
                readonly property real sourceTop: modelData.sourceTop
                readonly property real sourceBottom: modelData.sourceBottom
                readonly property real resultTop: modelData.resultTop
                readonly property real resultBottom: modelData.resultBottom
                readonly property real sourceX: connector.leftSide ? 0 : width
                readonly property real resultX: width - sourceX
                readonly property real middleX: width / 2
                // Current passages use the highlight; others use their kind's text-role tint.
                readonly property bool current: modelData.current
                readonly property color fill: current ? Kirigami.Theme.highlightColor : DiffStyle.textColor(kind, band)
                width: bandLayer.width
                height: bandLayer.height
                opacity: current ? 0.3 : DiffStyle.tint(kind)
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeWidth: -1
                    fillColor: band.fill
                    startX: band.sourceX
                    startY: band.sourceTop
                    // Horizontal tangents join the full row fills at both ends.
                    // Keep the midpoint controls in sync with ConnectorBands::bandAt.
                    PathCubic {
                        control1X: band.middleX; control1Y: band.sourceTop
                        control2X: band.middleX; control2Y: band.resultTop
                        x: band.resultX; y: band.resultTop
                    }
                    PathLine { x: band.resultX; y: band.resultBottom }
                    PathCubic {
                        control1X: band.middleX; control1Y: band.resultBottom
                        control2X: band.middleX; control2Y: band.sourceBottom
                        x: band.sourceX; y: band.sourceBottom
                    }
                    PathLine { x: band.sourceX; y: band.sourceTop }
                }
            }
        }
    }
    // Both edges belong to the handle's allocated width. Draw them over the
    // ribbon ends for the full split height, including the pane headers.
    Kirigami.Separator {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
    }
    Kirigami.Separator {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
    }
}
