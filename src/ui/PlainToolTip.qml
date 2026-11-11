// Tooltips render literal text regardless of the active controls style.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

ToolTip {
    id: tooltip
    // A style may retain its original text label after contentItem changes.
    // Keep that label empty and bind repository text only to the plain reader.
    property string literalText: ""
    text: ""
    x: {
        const anchor = parent
        return anchor ? Math.round((anchor.width - implicitWidth) / 2) : 0
    }
    contentItem: Text {
        text: tooltip.literalText
        font: tooltip.font
        color: tooltip.palette.toolTipText
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
}
