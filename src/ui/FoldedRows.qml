// The rows that stand for the result's folded conflict-marker blocks. A folded block keeps
// only its opening marker line in the layout, and its row lies over that line: the label
// "Unresolved conflict" and a Take both button for each section in the block whose sides can
// be combined. A button's menu takes both sides in either order through combinationChosen.
// The layer sits in the pane's Flickable above the TextArea, takes its rows from the pane's
// gutter and scrolls with the text; the gutter, declared after it, keeps the line number and
// the glyph of the row. A row is current, as LineBackgrounds draws a line, when one of the
// [first, end) ranges in current meets any line of its block.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: layer
    required property LineGutter gutter
    // Take both offers by row line, each a list of {id, label}.
    property var combinations: ({})
    // Whether the file takes choices now: it can be edited and no input is composing.
    property bool choosing: false
    property var current: []
    signal combinationChosen(target: string, action: int)
    signal choiceFocused(target: string, active: bool)
    z: 1
    function isCurrent(line: int, last: int): bool {
        for (let i = 0; i < current.length; ++i) {
            if (current[i][0] <= last && line < Math.max(current[i][1], current[i][0] + 1))
                return true
        }
        return false
    }
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    Repeater {
        model: layer.gutter.rowSlots
        delegate: Loader {
            id: slot
            required property int index
            readonly property var modelData: layer.gutter.rows[index] || layer.gutter.noRow
            readonly property string part: "row"
            readonly property int line: modelData.number
            readonly property var fold: layer.gutter.folds[line] || null
            readonly property var offers: layer.combinations[line] || []
            active: fold !== null
            visible: active
            y: modelData.top
            width: layer.width
            height: modelData.extent
            sourceComponent: Rectangle {
                id: row
                // Covers the opening marker line, which the buffer keeps.
                color: Kirigami.Theme.negativeBackgroundColor
                Accessible.role: Accessible.StaticText
                Accessible.name: conflictLabel.text
                Accessible.description: qsTr("Lines %1 to %2 are folded. Show conflict markers shows them.")
                    .arg(slot.line).arg(slot.fold ? slot.fold.last : slot.line)
                Rectangle {
                    anchors.fill: parent
                    visible: !!slot.fold && layer.isCurrent(slot.line, slot.fold.last)
                    color: Kirigami.Theme.highlightColor
                    opacity: 0.3
                }
                RowLayout {
                    // At the left edge of the text in view, however far the pane is scrolled.
                    x: layer.gutter.x + layer.gutter.width + Kirigami.Units.smallSpacing
                    height: row.height
                    spacing: Kirigami.Units.largeSpacing
                    Label {
                        id: conflictLabel
                        objectName: "resultConflictRow-" + slot.line
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("Unresolved conflict")
                        textFormat: Text.PlainText
                        font.weight: Font.DemiBold
                        color: Kirigami.Theme.negativeTextColor
                    }
                    Repeater {
                        model: slot.offers
                        delegate: ToolButton {
                            id: takeBoth
                            required property var modelData
                            readonly property string target: modelData.id
                            readonly property string label: qsTr("Take both: %1").arg(modelData.label)
                            objectName: "resultTakeBoth-" + target
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: row.height
                            enabled: layer.choosing
                            flat: false
                            focusPolicy: Qt.StrongFocus
                            topPadding: 0
                            bottomPadding: 0
                            display: AbstractButton.TextBesideIcon
                            text: qsTr("Take both")
                            icon.name: "merge"
                            icon.width: Math.min(Kirigami.Units.iconSizes.small, row.height)
                            icon.height: Math.min(Kirigami.Units.iconSizes.small, row.height)
                            down: pressed || takeBothMenu.visible
                            Accessible.role: Accessible.ButtonMenu
                            Accessible.name: label
                            ToolTip.text: label
                            ToolTip.visible: hovered && !takeBothMenu.visible
                            ToolTip.delay: Kirigami.Units.toolTipDelay
                            onActiveFocusChanged: layer.choiceFocused(target, activeFocus)
                            onClicked: takeBothMenu.popup(takeBoth, 0, takeBoth.height)
                            Menu {
                                id: takeBothMenu
                                MenuItem {
                                    objectName: "resultTakeBothLeftFirst-" + takeBoth.target
                                    text: qsTr("Take both, Left then Right")
                                    enabled: layer.choosing
                                    onTriggered: layer.combinationChosen(takeBoth.target, ChoicePreviewFlow.CombineLeftFirst)
                                }
                                MenuItem {
                                    objectName: "resultTakeBothRightFirst-" + takeBoth.target
                                    text: qsTr("Take both, Right then Left")
                                    enabled: layer.choosing
                                    onTriggered: layer.combinationChosen(takeBoth.target, ChoicePreviewFlow.CombineRightFirst)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
