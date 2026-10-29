// Confirms dropping a draft that is newer than the result staged in Git, so that staged
// result becomes current again. A refused or failed discard keeps the draft and says why here.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.PromptDialog {
    id: discardDialog
    required property ApplicationController controller
    // The file and revision the user confirms; the controller refuses a newer revision as stale.
    property string fileId: ""
    property string path: ""
    property string revision: ""
    property string failure: ""
    // Asks the owner to set plain text on an item and each label inside it.
    signal plainTextRequested(item: Item)
    // Both callers ask about the selected file, whose display path the controller has.
    function ask(id: string, current: string): void {
        fileId = id;
        path = id === controller.selectedFileId ? (controller.selectedFile.path || "") : "";
        revision = current;
        failure = "";
        open();
    }
    function discard(): void {
        if (controller.discardToStaged(fileId, revision))
            close();
        else
            failure = controller.commandMessage;
    }
    objectName: "discardStagedDialog"
    title: qsTr("Discard draft?")
    subtitle: qsTr("The draft of %1 is lost. The result staged in Git becomes the current result again, and the repository file stays as it is.")
        .arg(path)
    dialogType: Kirigami.PromptDialog.Warning
    standardButtons: Kirigami.Dialog.Discard | Kirigami.Dialog.Cancel
    // The subtitle names a repository path.
    onAboutToShow: plainTextRequested(contentItem)
    onDiscarded: discard()
    Component.onCompleted: {
        const button = standardButton(Kirigami.Dialog.Discard);
        button.objectName = "confirmDiscardStaged";
        button.text = qsTr("Discard draft");
        button.icon.name = "document-revert";
    }
    PlainInlineMessage {
        objectName: "discardStagedError"
        Layout.fillWidth: true
        onPlainTextRequested: item => discardDialog.plainTextRequested(item)
        visible: discardDialog.failure.length > 0
        type: Kirigami.MessageType.Error
        text: discardDialog.failure
    }
}
