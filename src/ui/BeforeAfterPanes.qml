// A consistently sized pair of read-only snapshots.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: pair
    property string namePrefix: ""
    property string beforeTitle: ""
    property string afterTitle: ""
    property string beforeText: ""
    property string afterText: ""
    Layout.fillWidth: true
    Layout.preferredHeight: Kirigami.Units.gridUnit * 8
    SnapshotPane {
        paneName: pair.namePrefix + "Before"
        paneTitle: pair.beforeTitle
        paneText: pair.beforeText
    }
    SnapshotPane {
        paneName: pair.namePrefix + "After"
        paneTitle: pair.afterTitle
        paneText: pair.afterText
    }
}
