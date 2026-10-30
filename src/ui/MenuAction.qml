// A toolbar button that opens a menu of its child entries. Each menu item takes its
// entry's objectName, so a test opens the menu and clicks the entry by name.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

NamedAction {
    id: menuAction
    // Keep our named button in overflow too. Kirigami otherwise turns Action.children
    // into its own unnamed submenu and bypasses this action's display component.
    default property list<MenuEntry> entries
    property Menu containingMenu: null
    // Overflow display delegates can be replaced while a popup is open. The action
    // owns the popup so its entries survive those layout and focus changes.
    readonly property Menu entryMenu: Menu {
        id: entryPopup
        focus: true
        z: menuAction.containingMenu ? menuAction.containingMenu.z + 1 : Kirigami.OverlayZStacking.z
        onOpened: forceActiveFocus()
        Instantiator {
            model: menuAction.entries
            delegate: MenuItem {
                required property MenuEntry modelData
                objectName: modelData.controlName
                action: modelData
                onTriggered: {
                    entryPopup.dismiss()
                    if (menuAction.containingMenu)
                        menuAction.containingMenu.dismiss()
                }
            }
            onObjectAdded: (index, entry) => entryPopup.insertItem(index, entry as MenuItem)
            onObjectRemoved: (index, entry) => entryPopup.removeItem(entry as MenuItem)
        }
    }
    displayComponent: ToolButton {
        id: menuButton
        readonly property Menu containingMenu: menuAction.ownerMenu(parent)
        objectName: menuAction.controlName
        text: menuAction.text
        icon.name: menuAction.icon.name
        enabled: menuAction.enabled
        display: menuAction.iconOnly && !containingMenu ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
        down: pressed || entryPopup.visible
        Accessible.role: containingMenu ? Accessible.MenuItem : Accessible.ButtonMenu
        Accessible.name: menuAction.accessibleName
        ToolTip.text: menuAction.tooltip
        ToolTip.visible: hovered && !entryPopup.visible && ToolTip.text.length > 0
        ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: {
            menuAction.containingMenu = containingMenu
            const overlay = Overlay.overlay
            const position = menuButton.mapToItem(overlay, 0, menuButton.height)
            entryPopup.popup(overlay, position.x, position.y)
        }
    }
}
