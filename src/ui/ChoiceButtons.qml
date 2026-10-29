// The choices on one row of a source pane's choice column: Accept and Ignore for the scope
// that starts on the row, and a menu with Resolve using this side, Take both in either order,
// and the choices of any other scope that starts on the same line, such as the line choice on
// a section's first line. Accept sits nearest the result. Once the scope is decided its
// buttons go and the pane's glyph column shows the decision; undo brings them back.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Row {
    id: buttons
    // {line, choice, extras}: the scope that owns the row and the other scopes on its line.
    // Each is a scoped choice with its region's undecided flag.
    required property var modelData
    // The pane edge that faces the result: "right" in the left pane, "left" in the right one.
    required property string edge
    readonly property bool pointsRight: edge === "right"
    required property real rowHeight
    // Whether the file takes choices now: it can be edited and no input is composing.
    required property bool choosing
    // A choice of action for a scope; the owner previews it when it needs confirming.
    signal chosen(target: string, action: int)
    signal hovered(target: string)
    signal focused(target: string, active: bool)
    readonly property var choice: modelData.choice
    readonly property real iconSize: Math.min(Kirigami.Units.iconSizes.small, rowHeight)
    // The menu entries: this scope's two-sided choices, then each other scope's choices.
    readonly property var entries: {
        const list = []
        const twoSided = function(scope, named) {
            list.push({name: "choiceResolve-" + scope.id, target: scope.id, action: ChoicePreviewFlow.ResolveUsingSide,
                enabled: !!scope.available,
                text: named ? qsTr("Resolve %1 using this side").arg(scope.label) : qsTr("Resolve using this side")})
            list.push({name: "choicePreviewLeftFirst-" + scope.id, target: scope.id, action: ChoicePreviewFlow.CombineLeftFirst,
                enabled: !!scope.combinationAvailable,
                text: named ? qsTr("Take both for %1, Left then Right").arg(scope.label)
                            : qsTr("Take both, Left then Right")})
            list.push({name: "choicePreviewRightFirst-" + scope.id, target: scope.id, action: ChoicePreviewFlow.CombineRightFirst,
                enabled: !!scope.combinationAvailable,
                text: named ? qsTr("Take both for %1, Right then Left").arg(scope.label)
                            : qsTr("Take both, Right then Left")})
        }
        twoSided(choice, false)
        const extras = modelData.extras || []
        for (let i = 0; i < extras.length; ++i) {
            const extra = extras[i]
            list.push({name: "choiceAccept-" + extra.id, target: extra.id, action: ChoicePreviewFlow.Accept,
                enabled: !!extra.available, text: qsTr("Accept %1 only").arg(extra.label)})
            list.push({name: "choiceIgnore-" + extra.id, target: extra.id, action: ChoicePreviewFlow.Ignore,
                enabled: !!extra.available, text: qsTr("Ignore %1 only").arg(extra.label)})
            twoSided(extra, true)
        }
        return list
    }
    readonly property bool hasEntries: entries.some(entry => entry.enabled)
    spacing: Kirigami.Units.smallSpacing
    layoutDirection: pointsRight ? Qt.RightToLeft : Qt.LeftToRight
    // A scope that cannot take a choice says why while the pointer rests on its disabled buttons.
    HoverHandler {
        id: rowHover
        onHoveredChanged: buttons.hovered(hovered ? buttons.choice.id : "")
    }
    readonly property bool choiceFocused: accept.activeFocus || ignore.activeFocus || more.activeFocus
    onChoiceFocusedChanged: focused(choice.id, choiceFocused)
    ToolTip.text: buttons.choice.detail || ""
    ToolTip.visible: rowHover.hovered && !more.hovered && buttons.choice.undecided && !buttons.choice.available
        && ToolTip.text.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay

    // A small framed icon button one row high; its text names the action and the scope.
    component GutterButton: ToolButton {
        required property real size
        required property real glyphSize
        width: size
        height: size
        padding: 0
        flat: false
        display: AbstractButton.IconOnly
        icon.width: glyphSize
        icon.height: glyphSize
        ToolTip.text: text
        ToolTip.visible: hovered
        ToolTip.delay: Kirigami.Units.toolTipDelay
    }
    GutterButton {
        id: accept
        size: buttons.rowHeight
        glyphSize: buttons.iconSize
        objectName: "choiceAccept-" + buttons.choice.id
        visible: !!buttons.choice.undecided
        enabled: buttons.choosing && !!buttons.choice.available
        text: qsTr("Accept %1").arg(buttons.choice.label)
        icon.name: buttons.pointsRight ? "arrow-right" : "arrow-left"
        icon.color: Kirigami.Theme.activeTextColor
        onClicked: buttons.chosen(buttons.choice.id, ChoicePreviewFlow.Accept)
    }
    GutterButton {
        id: ignore
        size: buttons.rowHeight
        glyphSize: buttons.iconSize
        objectName: "choiceIgnore-" + buttons.choice.id
        visible: !!buttons.choice.undecided
        enabled: buttons.choosing && !!buttons.choice.available
        text: qsTr("Ignore %1").arg(buttons.choice.label)
        icon.name: "edit-delete-remove"
        icon.color: Kirigami.Theme.negativeTextColor
        onClicked: buttons.chosen(buttons.choice.id, ChoicePreviewFlow.Ignore)
    }
    GutterButton {
        id: more
        size: buttons.rowHeight
        glyphSize: buttons.iconSize
        objectName: "choiceMore-" + buttons.choice.id
        visible: buttons.hasEntries
        enabled: buttons.choosing
        text: qsTr("More choices for %1").arg(buttons.choice.label)
        icon.name: "merge"
        down: pressed || entryMenu.visible
        Accessible.role: Accessible.ButtonMenu
        ToolTip.visible: hovered && !entryMenu.visible
        onClicked: entryMenu.popup(more, 0, more.height)
        Menu {
            id: entryMenu
            Instantiator {
                model: buttons.entries
                delegate: MenuItem {
                    required property var modelData
                    objectName: modelData.name
                    text: modelData.text
                    enabled: buttons.choosing && modelData.enabled
                    onTriggered: buttons.chosen(modelData.target, modelData.action)
                }
                onObjectAdded: (index, entry) => entryMenu.insertItem(index, entry as MenuItem)
                onObjectRemoved: (index, entry) => entryMenu.removeItem(entry as MenuItem)
            }
        }
    }
}
