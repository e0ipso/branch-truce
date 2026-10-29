// Repository and status text is always literal, with the full value on hover when elided.
import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

Label {
    id: label
    textFormat: Text.PlainText
    elide: Text.ElideRight
    HoverHandler { id: labelHover }
    ToolTip.text: label.text
    ToolTip.visible: labelHover.hovered && label.truncated
    ToolTip.delay: Kirigami.Units.toolTipDelay
}
