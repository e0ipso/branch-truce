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

    component CapturedSource: ColumnLayout {
        id: sourcePane
        required property string paneName
        required property string paneRole
        required property var source
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
        Flickable {
            objectName: sourcePane.paneName + "Viewport"
            Layout.fillWidth: true
            Layout.fillHeight: true
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
                wrapMode: TextEdit.NoWrap
                Accessible.name: sourcePane.paneRole + " · " + (sourcePane.source.label || "")
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
            Flickable {
                objectName: "resultViewport"
                Layout.fillWidth: true
                Layout.fillHeight: true
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
                    wrapMode: TextEdit.NoWrap
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
    }
    CapturedSource {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(comparison.height / 3, 180)
        visible: showEarlier.enabled && showEarlier.checked
        alignHeader: false
        paneName: "earlier"
        paneRole: qsTr("Earlier-version context · read-only")
        source: comparison.file.earlier || ({})
    }
}
