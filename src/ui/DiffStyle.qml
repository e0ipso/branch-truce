// The visual and spoken cues for each passage kind, using the caller's theme.
pragma Singleton

import QtQuick
import org.kde.kirigami as Kirigami

QtObject {
    id: styles
    readonly property var entries: ({
        added:    {role: "positive", glyph: "+", tint: 0.4, description: qsTr("Added line"), fill: true},
        changed:  {role: "active", glyph: "~", tint: 0.4, description: qsTr("Changed line"), fill: true},
        removed:  {role: "disabled", glyph: "−", tint: 0.3, description: qsTr("Removed line"), fill: false},
        conflict: {role: "negative", glyph: "!", tint: 0.4, description: qsTr("Undecided conflict"), fill: true},
        manual:   {role: "neutral", glyph: "m", tint: 0.4, description: qsTr("Hand-edited line"), fill: true},
        accepted: {role: "positive", glyph: "✓", tint: 0.4, description: qsTr("Accepted side"), fill: true},
        ignored:  {role: "disabled", glyph: "×", tint: 0.3, description: qsTr("Ignored side"), fill: false},
        markers:  {role: "negative", glyph: "!", tint: 0.4, description: qsTr("Conflict markers remain"), fill: true}
    })
    readonly property var fallback: ({role: "neutral", glyph: "?", tint: 0.4,
        description: qsTr("Unknown change"), fill: true})
    function entry(kind: string): var {
        return Object.prototype.hasOwnProperty.call(entries, kind) ? entries[kind] : fallback
    }
    function glyph(kind: string): string { return entry(kind).glyph }
    function description(kind: string): string { return entry(kind).description }
    function tint(kind: string): real { return entry(kind).tint }
    function hasFill(kind: string): bool { return entry(kind).fill }
    function pillTone(kind: string): string {
        const role = entry(kind).role
        return role === "neutral" ? "warning" : role === "disabled" ? "neutral" : role
    }
    function textColor(kind: string, item: Item): color {
        switch (entry(kind).role) {
        case "positive": return item.Kirigami.Theme.positiveTextColor
        case "active": return item.Kirigami.Theme.activeTextColor
        case "negative": return item.Kirigami.Theme.negativeTextColor
        case "disabled": return item.Kirigami.Theme.disabledTextColor
        default: return item.Kirigami.Theme.neutralTextColor
        }
    }
    function background(kind: string, item: Item): color {
        switch (entry(kind).role) {
        case "positive": return item.Kirigami.Theme.positiveBackgroundColor
        case "active": return item.Kirigami.Theme.activeBackgroundColor
        case "negative": return item.Kirigami.Theme.negativeBackgroundColor
        case "disabled": return item.Kirigami.Theme.disabledTextColor
        default: return item.Kirigami.Theme.neutralBackgroundColor
        }
    }
}
