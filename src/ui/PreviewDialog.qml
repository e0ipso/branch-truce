// Common shell for a change that is confirmed after reviewing its preview.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Dialog {
    id: preview
    anchors.centerIn: parent
    x: 0
    y: 0
    required property ChoicePreviewFlow choices
    required property int scope
    property string path: ""
    property string summary: ""
    property string detail: ""
    property string detailName: ""
    property string confirmText: ""
    // Asks the owner to apply the previewed change. A handler that cannot
    // apply it sets refused, which keeps the dialog open and shows why.
    signal applyRequested()
    // The message a refused apply left, shown inside the dialog.
    property string refusalMessage: ""
    // An input method is composing in the result; applying waits for it.
    property bool composing: false
    property bool refused: false
    property bool pending: false
    property string pendingRequest: ""
    default property alias entries: entryColumn.data
    onAboutToShow: {
        refused = false;
        pending = false;
    }
    // Applies the preview and closes only when it applied.
    function confirm(): void {
        if (pending)
            return;
        pending = true;
        pendingRequest = choices.previewRequest;
        refused = false;
        applyRequested();
        if (refused)
            pending = false;
    }
    Connections {
        target: preview.choices
        function onApplyFinished(scope: int, request: string, succeeded: bool): void {
            if (!preview.pending || scope !== preview.scope || request !== preview.pendingRequest)
                return;
            preview.pending = false;
            preview.refused = !succeeded;
            if (succeeded)
                preview.close();
        }
    }
    preferredWidth: Kirigami.Units.gridUnit * 52
    padding: Kirigami.Units.largeSpacing
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            text: preview.confirmText
            enabled: !preview.composing && !preview.pending
            onTriggered: preview.confirm()
        }
    ]
    dialogData: [
        PreviewBody {
            dialog: preview
            Kirigami.InlineMessage {
                objectName: "previewRefusal"
                Layout.fillWidth: true
                type: Kirigami.MessageType.Error
                visible: preview.refused && preview.refusalMessage.length > 0
                text: preview.refusalMessage
            }
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
