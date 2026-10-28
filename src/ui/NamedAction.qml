// A toolbar action whose button keeps an objectName, so tests find it by name and not
// by its text. While the action fits, the toolbar shows this button instead of its own.
// iconOnly hides the text here: Kirigami.DisplayHint.IconOnly would make the toolbar show
// its own unnamed icon button instead.
// A command button mirrors its action instead of taking it as its `action`. The toolbar keeps
// its own checkable button on the same action and that button makes the action checkable, so
// a button bound to the action would report a check box to assistive technology and keep the
// checked look of a command that was clicked. Only a toggle binds its action, and is checkable.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Kirigami.Action {
    id: namedAction
    required property string controlName
    property string accessibleName: text
    property bool iconOnly: false
    property bool toggle: false
    checkable: toggle
    displayComponent: ToolButton {
        objectName: namedAction.controlName
        action: namedAction.toggle ? namedAction : null
        text: namedAction.text
        icon.name: namedAction.icon.name
        enabled: namedAction.enabled
        checkable: namedAction.toggle
        onClicked: if (!namedAction.toggle) namedAction.trigger()
        display: namedAction.iconOnly ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
        Accessible.name: namedAction.accessibleName
        ToolTip.text: namedAction.tooltip || (display === AbstractButton.IconOnly ? namedAction.text : "")
        ToolTip.visible: hovered && ToolTip.text.length > 0
        ToolTip.delay: Kirigami.Units.toolTipDelay
    }
}
