// A dialog body that sizes the dialog to its rows. Kirigami.Dialog keeps the content
// height it measured at creation when that was zero, and the handoff has no rows until
// the session loads, so the body never measures zero.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    default property alias content: dialogColumn.data
    implicitWidth: dialogColumn.implicitWidth
    implicitHeight: Math.max(Kirigami.Units.gridUnit, dialogColumn.implicitHeight)
    ColumnLayout {
        id: dialogColumn
        width: parent.width
        spacing: Kirigami.Units.largeSpacing
    }
}
