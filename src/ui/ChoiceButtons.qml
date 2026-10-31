// The choices on one row of a source pane's choice column: Accept and Ignore for the scope
// that starts on the row, and a context menu with Resolve using this side, Take both in either order,
// and every precise line choice in that actual source range.
// Accept sits nearest the result. Once the scope is decided its
// buttons go and the pane's glyph column shows the decision; undo brings them back.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Row {
    id: buttons
    // {line, choice, extras}: the range's section and all its precise line scopes.
    // Each is a scoped choice with its region's undecided flag.
    required property var modelData
    // The pane edge that faces the result: "right" in the left pane, "left" in the right one.
    required property string edge
    readonly property bool pointsRight: edge === "right"
    required property real rowHeight
    // Whether the file takes choices now: it can be edited and no input is composing.
    required property bool choosing
    property TextArea contextEditor: null
    property EditActions edits: null
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
    // A decided section may still contain an undecided precise line. Only a
    // fully decided range loses its persistent controls. An unavailable owner
    // still needs a keyboard/touch target for its available mapped lines.
    readonly property bool rangeUndecided: !!choice.undecided
        || (modelData.extras || []).some(extra => !!extra.undecided)
    readonly property bool menuOnly: rangeUndecided && hasEntries && (!choice.undecided || !choice.available)
    readonly property string menuLabel: qsTr("More choices for %1").arg(choice.label)
    readonly property string menuHint: qsTr("Right-click, hold, or press Menu or Shift+F10 for more choices.")
    function openMenu(): void {
        if (!rangeUndecided || !visible)
            return
        const anchor = contextEditor || buttons
        const point = buttons.mapToItem(anchor, 0, rowHeight)
        openMenuAt(anchor, point.x, point.y)
    }
    // Source text is a stable context-menu anchor even after the range is
    // decided. No hidden gutter button is kept alive as a menu target.
    function openMenuAt(anchor: Item, x: real, y: real): bool {
        if (!choosing || !hasEntries || !anchor || !anchor.visible)
            return false
        entryMenu.popup(anchor, x, y)
        return true
    }
    function choose(target: string, action: int): void {
        if (entryMenu)
            entryMenu.close()
        if (contextEditor)
            contextEditor.forceActiveFocus(Qt.OtherFocusReason)
        hovered("")
        focused(choice.id, false)
        chosen(target, action)
    }
    onRangeUndecidedChanged: if (!rangeUndecided) {
        const restoreFocus = choiceFocused
        if (entryMenu)
            entryMenu.close()
        hovered("")
        focused(choice.id, false)
        if (restoreFocus && contextEditor)
            contextEditor.forceActiveFocus(Qt.OtherFocusReason)
    }
    Component.onDestruction: {
        hovered("")
        focused(choice.id, false)
    }
    spacing: Kirigami.Units.smallSpacing
    layoutDirection: pointsRight ? Qt.RightToLeft : Qt.LeftToRight
    // A scope that cannot take a choice says why while the pointer rests on its disabled buttons.
    HoverHandler {
        id: rowHover
        enabled: buttons.rangeUndecided && buttons.visible
        onHoveredChanged: buttons.hovered(hovered ? buttons.choice.id : "")
    }
    readonly property bool choiceFocused: accept.activeFocus || ignore.activeFocus || more.activeFocus || entryMenu.visible
    onChoiceFocusedChanged: focused(choice.id, choiceFocused)
    ToolTip.text: buttons.choice.detail || ""
    ToolTip.visible: rowHover.hovered && !entryMenu.visible && buttons.choice.undecided && !buttons.choice.available
        && ToolTip.text.length > 0
    ToolTip.delay: Kirigami.Units.toolTipDelay

    TapHandler {
        enabled: buttons.rangeUndecided && buttons.visible
        acceptedButtons: Qt.RightButton
        onTapped: buttons.openMenu()
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Menu || event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
            buttons.openMenu()
            event.accepted = true
        }
    }

    // A small framed icon button within one row; its text names the action and the scope.
    component GutterButton: ToolButton {
        required property real size
        required property real glyphSize
        width: glyphSize
        height: size
        padding: 0
        flat: false
        display: AbstractButton.IconOnly
        icon.width: glyphSize
        icon.height: glyphSize
        Accessible.description: buttons.menuHint
        ToolTip.text: text + "\n" + buttons.menuHint
        ToolTip.visible: hovered && !entryMenu.visible
        ToolTip.delay: Kirigami.Units.toolTipDelay
        onPressAndHold: buttons.openMenu()
    }
    GutterButton {
        id: accept
        size: buttons.rowHeight
        glyphSize: buttons.iconSize
        objectName: "choiceAccept-" + buttons.choice.id
        visible: !!buttons.choice.undecided && !buttons.menuOnly
        enabled: buttons.choosing && !!buttons.choice.available
        text: qsTr("Accept %1").arg(buttons.choice.label)
        icon.name: buttons.pointsRight ? "arrow-right" : "arrow-left"
        icon.color: Kirigami.Theme.activeTextColor
        onClicked: buttons.choose(buttons.choice.id, ChoicePreviewFlow.Accept)
    }
    GutterButton {
        id: ignore
        size: buttons.rowHeight
        glyphSize: buttons.iconSize
        objectName: "choiceIgnore-" + buttons.choice.id
        visible: !!buttons.choice.undecided && !buttons.menuOnly
        enabled: buttons.choosing && !!buttons.choice.available
        text: qsTr("Ignore %1").arg(buttons.choice.label)
        icon.name: "edit-delete-remove"
        icon.color: Kirigami.Theme.negativeTextColor
        onClicked: buttons.choose(buttons.choice.id, ChoicePreviewFlow.Ignore)
    }
    GutterButton {
        id: more
        size: buttons.rowHeight
        glyphSize: buttons.iconSize
        // An undecided unmapped owner can still have available line choices. Keep
        // their menu focusable in the same lane without adding a third button.
        objectName: "choiceMenu-" + buttons.choice.id
        visible: buttons.menuOnly
        enabled: buttons.choosing
        text: buttons.menuLabel
        icon.name: "overflow-menu"
        down: pressed || entryMenu.visible
        Accessible.role: Accessible.ButtonMenu
        onClicked: buttons.openMenu()
    }
    Menu {
        id: entryMenu
        objectName: "choiceMore-" + buttons.choice.id
        title: buttons.menuLabel
        Accessible.name: buttons.menuLabel
        MenuItem {
            text: buttons.choice.label
            enabled: false
            Accessible.description: buttons.choice.detail || ""
        }
        MenuItem {
            visible: !!buttons.edits
            action: buttons.edits ? buttons.edits.undo : null
        }
        MenuItem {
            visible: !!buttons.edits
            action: buttons.edits ? buttons.edits.redo : null
        }
        MenuItem {
            text: qsTr("Copy")
            enabled: !!buttons.contextEditor && buttons.contextEditor.selectedText.length > 0
            onTriggered: buttons.contextEditor.copy()
        }
        MenuItem {
            text: qsTr("Select all")
            enabled: !!buttons.contextEditor
            onTriggered: buttons.contextEditor.selectAll()
        }
        MenuSeparator {}
        Instantiator {
            model: buttons.entries
            delegate: MenuItem {
                required property var modelData
                objectName: modelData.name
                text: modelData.text
                enabled: buttons.choosing && modelData.enabled
                onTriggered: buttons.choose(modelData.target, modelData.action)
            }
            onObjectAdded: (index, entry) => entryMenu.insertItem(index + 6, entry as MenuItem)
            onObjectRemoved: (index, entry) => entryMenu.removeItem(entry as MenuItem)
        }
    }
}
