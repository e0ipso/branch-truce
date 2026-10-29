// A scope label and its before-and-after snapshots.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: change
    property string scope: ""
    property string namePrefix: ""
    property string beforeTitle: ""
    property string afterTitle: ""
    property string beforeText: ""
    property string afterText: ""
    spacing: Kirigami.Units.smallSpacing
    PlainLabel {
        Layout.fillWidth: true
        text: change.scope
        wrapMode: Text.Wrap
    }
    BeforeAfterPanes {
        namePrefix: change.namePrefix
        beforeTitle: change.beforeTitle
        afterTitle: change.afterTitle
        beforeText: change.beforeText
        afterText: change.afterText
    }
}
