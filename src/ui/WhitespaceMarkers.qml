// Markers for the spaces and tabs in a text pane: a dot per space and a line per tab.
// Qt Quick draws its text without the dots and arrows that QTextOption.ShowTabsAndSpaces
// asks for, so the markers are drawn over the text. They are not document characters.
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: markers
    required property TextEdit editor
    required property Flickable viewport
    // The pane's blocks, which find the markers. A hidden line gets none.
    required property TextBlocks blocks
    property bool active: false
    property var spots: []
    readonly property color ink: editor.color.a > 0 ? editor.color : Kirigami.Theme.textColor
    readonly property real stroke: Kirigami.Units.smallSpacing / 2
    readonly property real inset: Kirigami.Units.smallSpacing / 4
    // A space's dot is this share of its line's height, kept between the stroke and the small spacing.
    readonly property real dotShare: 0.22
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
        // This item's area in document coordinates, which start inside the editor's padding.
        const origin = editor.mapFromItem(markers, 0, 0)
        spots = blocks.whitespaceWithin(Qt.rect(origin.x - editor.leftPadding, origin.y - editor.topPadding,
            width, height), stroke, Kirigami.Units.largeSpacing)
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
    Item {
        objectName: markers.objectName + "Spots"
        anchors.fill: parent
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
                    width: Math.min(Kirigami.Units.smallSpacing, Math.max(markers.stroke, spot.height * markers.dotShare))
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
}
