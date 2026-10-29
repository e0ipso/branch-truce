// Common shell for a change that is confirmed after reviewing its preview.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: preview
    property string path: ""
    property string summary: ""
    property string detail: ""
    property string detailName: ""
    property string confirmText: ""
    default property alias entries: entryColumn.data
    signal confirmed()
    preferredWidth: Kirigami.Units.gridUnit * 52
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            text: preview.confirmText
            onTriggered: {
                preview.confirmed()
                preview.close()
            }
        }
    ]
    dialogData: [
        PreviewBody {
            dialog: preview
            PlainLabel {
                Layout.fillWidth: true
                visible: text.length > 0
                text: preview.path
                elide: Text.ElideMiddle
                font.bold: true
            }
            PlainLabel {
                Layout.fillWidth: true
                visible: text.length > 0
                text: preview.summary
                wrapMode: Text.Wrap
            }
            PlainLabel {
                objectName: preview.detailName
                Layout.fillWidth: true
                visible: text.length > 0
                text: preview.detail
                wrapMode: Text.Wrap
            }
            ColumnLayout {
                id: entryColumn
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.largeSpacing
            }
        }
    ]
}
