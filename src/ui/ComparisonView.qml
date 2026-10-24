pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
        required property Item editor
        required property Flickable viewport
        property bool active: false
        property bool wrapped: false
        property var spots: []
        readonly property int markerCount: spots.length
        readonly property color ink: editor.color.a > 0 ? editor.color : "#202020"
        anchors.fill: parent
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
            if (!active || width < 1 || height < 1) {
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
                    if (Math.abs(mappedNext.y - mapped.y) < 1)
                        span = mappedNext.x - mapped.x
                }
                if (span < 2)
                    span = ch === "\t" ? 8 : 2
                if ((ch === " " || ch === "\t") && mapped.y + rect.height >= 0
                        && mapped.x < width && mapped.x + span > 0)
                    found.push({x: mapped.x, y: mapped.y, w: span, h: Math.max(rect.height, 2), tab: ch === "\t"})
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
                width: Math.max(2, modelData.w)
                height: Math.max(2, modelData.h)
                Rectangle {
                    visible: !spot.modelData.tab
                    width: Math.min(4, Math.max(2, spot.height * 0.22))
                    height: width
                    radius: width / 2
                    color: markers.ink
                    anchors.centerIn: parent
                }
                Rectangle {
                    visible: spot.modelData.tab
                    x: 1
                    height: 2
                    width: Math.max(1, parent.width - 8)
                    anchors.verticalCenter: parent.verticalCenter
                    color: markers.ink
                }
                Rectangle {
                    visible: spot.modelData.tab
                    width: 5
                    height: 5
                    radius: 1
                    anchors.right: parent.right
                    anchors.rightMargin: 1
                    anchors.verticalCenter: parent.verticalCenter
                    color: markers.ink
                }
            }
        }
    }

    component CapturedSource: ColumnLayout {
        id: sourcePane
        required property string paneName
        required property string paneRole
        required property var source
        property alias editor: sourceText
        property bool alignHeader: true
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
                    font.family: "monospace"
                    width: comparison.wrapLines ? sourceViewport.width : implicitWidth
                    wrapMode: comparison.wrapLines ? TextEdit.Wrap : TextEdit.NoWrap
                    Accessible.name: sourcePane.paneRole + " · " + (sourcePane.source.label || "")
                    onTextChanged: if (comparison.whitespaceReady)
                        comparison.applyWhitespace()
                }
            }
            WhitespaceMarkers {
                objectName: sourcePane.paneName + "Whitespace"
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
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            ComboBox {
                id: searchScope
                objectName: "searchScope"
                model: [qsTr("Left source"), qsTr("Editable result"), qsTr("Right source")]
                Accessible.name: qsTr("Search scope")
                Layout.preferredWidth: Math.max(implicitWidth, 180)
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
        orientation: Qt.Horizontal
        CapturedSource {
            id: leftPane
            SplitView.minimumWidth: 140
            SplitView.preferredWidth: panes.width / 3
            paneName: "stage2"
            paneRole: qsTr("Left source")
            source: comparison.file.stage2 || ({})
        }
        ColumnLayout {
            SplitView.minimumWidth: 180
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
                        font.family: "monospace"
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
                }
                WhitespaceMarkers {
                    objectName: "resultWhitespace"
                    editor: resultEditor
                    viewport: resultViewport
                    active: comparison.showWhitespace
                    wrapped: comparison.wrapLines
                }
            }
        }
        CapturedSource {
            id: rightPane
            SplitView.minimumWidth: 140
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
        Layout.preferredHeight: Math.min(comparison.height / 3, 180)
        visible: showEarlier.enabled && showEarlier.checked
        alignHeader: false
        paneName: "earlier"
        paneRole: qsTr("Earlier-version context · read-only")
        source: comparison.file.earlier || ({})
    }
}
