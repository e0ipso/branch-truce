// A titled, read-only monospace text pane that shows a snapshot of text in a preview or notice.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: pane
    required property string paneName
    required property string paneTitle
    required property string paneText
    Layout.fillWidth: true
    Layout.fillHeight: true
    spacing: Kirigami.Units.smallSpacing
    TextMetrics { id: snapshotTabs; font: Kirigami.Theme.fixedWidthFont; text: "    " }
    PlainLabel {
        objectName: pane.paneName + "Label"
        Layout.fillWidth: true
        text: pane.paneTitle
        wrapMode: Text.WrapAnywhere
    }
    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        TextArea {
            objectName: pane.paneName + "Text"
            text: pane.paneText
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            font: Kirigami.Theme.fixedWidthFont
            tabStopDistance: snapshotTabs.advanceWidth
            wrapMode: TextEdit.NoWrap
            Accessible.name: pane.paneTitle
        }
    }
}
