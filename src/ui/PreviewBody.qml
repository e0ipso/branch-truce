// The body of a preview dialog. It takes the height the dialog may grow to, up to 32 grid
// units, so its panes fill the dialog rather than shrinking to their implicit height.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    required property Kirigami.Dialog dialog
    default property alias content: bodyColumn.data
    implicitHeight: Math.max(Kirigami.Units.gridUnit * 12, Math.min(Kirigami.Units.gridUnit * 32,
        dialog.maximumHeight - dialog.implicitHeaderHeight - dialog.implicitFooterHeight
        - dialog.topPadding - dialog.bottomPadding - Kirigami.Units.largeSpacing * 2))
    ColumnLayout {
        id: bodyColumn
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing
    }
}
