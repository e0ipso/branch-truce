// A toolbar button that opens a menu of its child entries. Each menu item takes its
// entry's objectName, so a test opens the menu and clicks the entry by name.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

NamedAction {
    id: menuAction
    displayComponent: ToolButton {
        id: menuButton
        objectName: menuAction.controlName
        text: menuAction.text
        icon.name: menuAction.icon.name
        enabled: menuAction.enabled
        down: pressed || entries.visible
        Accessible.role: Accessible.ButtonMenu
        Accessible.name: menuAction.accessibleName
        ToolTip.text: menuAction.tooltip
        ToolTip.visible: hovered && !entries.visible && ToolTip.text.length > 0
        ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: entries.popup(menuButton, 0, menuButton.height)
        Menu {
            id: entries
            Instantiator {
                model: menuAction.children
                delegate: MenuItem {
                    required property MenuEntry modelData
                    objectName: modelData.controlName
                    action: modelData
                }
                onObjectAdded: (index, entry) => entries.insertItem(index, entry as MenuItem)
                onObjectRemoved: (index, entry) => entries.removeItem(entry as MenuItem)
            }
        }
    }
}
