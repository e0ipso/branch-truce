pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Item {
    id: root
    width: 900
    height: 500
    property bool wrapText: false
    property bool earlierVisible: false
    property string statusText: "Experiment: not initialized"
    Row {
        anchors.fill: parent
        Repeater {
            model: ["Source A: captured stage 2", "Editable result", "Source B: captured stage 3"]
            Column {
                id: pane
                required property int index
                required property string modelData
                width: 300
                Label { text: pane.modelData }
                Label { text: "L1  Accept →  × Ignore | section" }
                TextArea {
                    objectName: pane.index === 1 ? "result" : (pane.index === 0 ? "sourceA" : "sourceB")
                    width: 290
                    height: 360
                    readOnly: pane.index !== 1
                    textFormat: TextEdit.PlainText
                    wrapMode: root.wrapText ? TextEdit.Wrap : TextEdit.NoWrap
                    selectByMouse: true
                    color: Kirigami.Theme.textColor
                    font.family: "monospace"
                    font.pixelSize: 16
                }
            }
        }
    }
    Label {
        objectName: "status"
        y: 400
        text: root.statusText
    }
    Label {
        objectName: "earlier"
        anchors.bottom: parent.bottom
        visible: root.earlierVisible
        text: "Earlier version: captured base, hidden without replacing the result"
    }
}
