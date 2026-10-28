// Markers for the spaces and tabs in a text pane: a dot per space and a line per tab.
// Qt Quick ignores QTextOption.ShowTabsAndSpaces, so the markers are drawn
// over the text. They are not document characters.
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: markers
    required property TextEdit editor
    required property Flickable viewport
    // The pane's blocks when it can hide lines, which get no markers.
    property TextBlocks blocks: null
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
            if (markers.blocks && markers.blocks.hiddenAt(pos)) {
                const shown = markers.blocks.shownPosition(pos, true)
                if (shown <= pos)
                    break
                pos = shown
                continue
            }
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
