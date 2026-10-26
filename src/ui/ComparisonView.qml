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
        searchDetail = qsTr("Passage in %1.").arg(scope)
    }
    Component.onCompleted: {
        refreshSearchScopes()
        applyWhitespace()
        whitespaceReady = true
    }

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
        readonly property color ink: editor.color.a > 0 ? editor.color : "#202020"
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
    // markerSlot is a column between the numbers and the text, kept free for
    // per-line glyphs and per-region actions. Place an item in it at the y of
    // editor.positionToRectangle(position); rows lists the blocks near the view
    // as {number, position, y, height}.
    component LineGutter: Item {
        id: gutter
        required property TextArea editor
        required property Flickable viewport
        property var rows: []
        readonly property alias markerSlot: slot
        readonly property int digits: Math.max(2, String(blocks.blockCount).length)
        readonly property real numberWidth: Math.ceil(digits * gutterFont.averageCharacterWidth)
        // The editor range, with one view height either side, that rows covers.
        property real coveredTop: 0
        property real coveredBottom: -1
        z: 1
        implicitWidth: Kirigami.Units.smallSpacing * 2 + numberWidth + slot.width + separator.width
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
                    y: editor.y + rect.y, height: rect.height}
                same = same && rows[i].number === row.number && rows[i].position === row.position
                    && rows[i].y === row.y && rows[i].height === row.height
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
        Item {
            id: slot
            objectName: gutter.objectName + "MarkerSlot"
            x: Kirigami.Units.smallSpacing * 2 + gutter.numberWidth
            width: Kirigami.Units.gridUnit
            height: gutter.height
        }
        Kirigami.Separator {
            id: separator
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
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
        property bool alignHeader: true
        property int minimumLines: comparison.minimumPaneLines
        readonly property real headerImplicitHeight: sourceHeader.implicitHeight
        Label {
            id: sourceHeader
            objectName: sourcePane.paneName + "Label"
            Layout.fillWidth: true
            Layout.preferredHeight: sourcePane.alignHeader ? comparison.headerHeight : implicitHeight
            text: sourcePane.paneRole + "\n"
                + (sourcePane.source.label || qsTr("Source identity unavailable"))
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        Label {
            objectName: sourcePane.paneName + "Identity"
            Layout.fillWidth: true
            text: qsTr("Captured blob: %1").arg(sourcePane.source.objectId || qsTr("unavailable"))
            textFormat: Text.PlainText
            elide: Text.ElideRight
            HoverHandler { id: identityHover }
            ToolTip.visible: identityHover.hovered
            ToolTip.text: qsTr("Verified commit: %1\nCaptured blob: %2")
                .arg(sourcePane.source.commitId || qsTr("unavailable"))
                .arg(sourcePane.source.objectId || qsTr("unavailable"))
            Accessible.description: ToolTip.text
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
                ScrollBar.vertical: ScrollBar {}
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
                }
                LineGutter {
                    id: sourceGutter
                    objectName: sourcePane.paneName + "Gutter"
                    x: sourceViewport.contentX
                    width: implicitWidth
                    height: Math.max(sourceText.height, sourceViewport.height)
                    editor: sourceText
                    viewport: sourceViewport
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
                text: qsTr("Editable result\nApp draft")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Initial content and your edits are kept")
                elide: Text.ElideRight
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
                    ScrollBar.vertical: ScrollBar {}
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
    }
}
