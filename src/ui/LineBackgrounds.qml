// Diff colours for one pane, painted under its text rather than as
// character formats. The layer sits in the pane's Flickable below the
// TextArea, takes its rows and marks from the pane's gutter and scrolls
// with the text. A gap is a thin rule on a line boundary. current lists
// the [first, end) line ranges of the navigation target, drawn on top.
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami

Item {
    id: backgrounds
    required property LineGutter gutter
    property var current: []
    readonly property real rule: Kirigami.Units.smallSpacing / 2
    z: -1
    enabled: false
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    function isCurrent(line: int): bool {
        for (let i = 0; i < current.length; ++i) {
            if (line >= current[i][0] && line < current[i][1])
                return true
        }
        return false
    }
    function isCurrentGap(line: int): bool {
        for (let i = 0; i < current.length; ++i) {
            if (current[i][0] === line && current[i][1] === line)
                return true
        }
        return false
    }
    // Kirigami stops a disabled item from inheriting its parent's theme and gives
    // it the default Window set, so each fill under this disabled layer names
    // the View set itself.
    component Fill: Rectangle {
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false
    }
    Repeater {
        model: backgrounds.gutter.rows
        delegate: Fill {
            required property var modelData
            readonly property string part: "line"
            readonly property int line: modelData.number
            readonly property string kind: backgrounds.gutter.marks.lines[line] || ""
            visible: kind.length > 0
            y: modelData.top
            width: backgrounds.width
            height: modelData.extent
            // An ignored side keeps its text readable under a faint tint.
            color: kind === "added" || kind === "accepted" ? Kirigami.Theme.positiveBackgroundColor
                : kind === "changed" ? Kirigami.Theme.activeBackgroundColor
                : kind === "conflict" || kind === "markers" ? Kirigami.Theme.negativeBackgroundColor
                : kind === "ignored" ? Kirigami.Theme.disabledTextColor
                : Kirigami.Theme.neutralBackgroundColor
            opacity: kind === "ignored" ? 0.15 : 1
        }
    }
    // A rule where folded marker lines are, so the sides of a block stay apart.
    Repeater {
        model: backgrounds.gutter.rows
        delegate: Fill {
            required property var modelData
            readonly property string part: "fold"
            readonly property int line: modelData.number
            readonly property string kind: backgrounds.gutter.folds[line] ? "markers" : ""
            visible: kind.length > 0
            y: modelData.top - height / 2
            width: backgrounds.width
            height: backgrounds.rule
            color: Kirigami.Theme.negativeTextColor
        }
    }
    Repeater {
        model: backgrounds.gutter.rows
        delegate: Fill {
            required property var modelData
            readonly property string part: "gap"
            readonly property int line: modelData.number
            readonly property string kind: backgrounds.gutter.marks.gaps[line] ? "removed" : ""
            visible: kind.length > 0
            y: modelData.top - height / 2
            width: backgrounds.width
            height: backgrounds.rule
            color: Kirigami.Theme.disabledTextColor
        }
    }
    Repeater {
        model: backgrounds.gutter.rows
        delegate: Fill {
            required property var modelData
            readonly property string part: "current"
            readonly property int line: modelData.number
            readonly property bool gap: backgrounds.isCurrentGap(line)
            visible: gap || backgrounds.isCurrent(line)
            y: gap ? modelData.top - height / 2 : modelData.top
            width: backgrounds.width
            height: gap ? backgrounds.rule * 2 : modelData.extent
            color: Kirigami.Theme.highlightColor
            opacity: 0.3
        }
    }
}
