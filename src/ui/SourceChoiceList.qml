// The selected file's source scopes, each with its text and the choices it allows, and the
// notice about those choices.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: sourceChoiceList
    required property var selectedFile
    required property bool supportedSelection
    // Whether the user asked to show the source choices.
    required property bool shown
    // True while an input method composes text that the result has not acknowledged.
    required property bool composing
    // Asks the owner to set plain text on a message and each label inside it, once they exist.
    signal plainTextRequested(item: Item)
    readonly property bool listShown: supportedSelection && shown
    readonly property bool noticeShown: shown && !!selectedFile.choiceNotice
    // A choice of action for a source scope; the caller previews it when it needs confirming.
    signal chosen(target: string, action: string)
    // A visible column with nothing to show would still take a share of the space and a
    // spacing in the page layout.
    visible: listShown || noticeShown
    Layout.fillWidth: true
    ListView {
        objectName: "scopedChoices"
        visible: sourceChoiceList.listShown
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 180)
        clip: true
        model: sourceChoiceList.selectedFile.choices || []
        delegate: ColumnLayout {
            id: choiceRow
            required property var modelData
            width: ListView.view.width
            Label {
                Layout.fillWidth: true
                text: choiceRow.modelData.label + " · " + choiceRow.modelData.state
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: choiceRow.modelData.sourceText || qsTr("Empty source scope · deletion")
                textFormat: Text.PlainText
                font.family: "monospace"
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            RowLayout {
                enabled: !!sourceChoiceList.selectedFile.canEdit && !!choiceRow.modelData.available && !sourceChoiceList.composing
                Button {
                    objectName: "choiceAccept-" + choiceRow.modelData.id
                    text: qsTr("Accept →")
                    Accessible.name: qsTr("Accept %1").arg(choiceRow.modelData.label)
                    onClicked: sourceChoiceList.chosen(choiceRow.modelData.id, "accept")
                }
                Button {
                    objectName: "choiceIgnore-" + choiceRow.modelData.id
                    text: qsTr("× Ignore")
                    Accessible.name: qsTr("Ignore %1").arg(choiceRow.modelData.label)
                    onClicked: sourceChoiceList.chosen(choiceRow.modelData.id, "ignore")
                }
                Button {
                    objectName: "choiceResolve-" + choiceRow.modelData.id
                    text: qsTr("Resolve using this side")
                    Accessible.name: qsTr("Resolve %1 using this side and ignore its competitor").arg(choiceRow.modelData.label)
                    onClicked: sourceChoiceList.chosen(choiceRow.modelData.id, "resolve")
                }
            }
            RowLayout {
                enabled: !!sourceChoiceList.selectedFile.canEdit && !!choiceRow.modelData.combinationAvailable && !sourceChoiceList.composing
                Button {
                    objectName: "choicePreviewLeftFirst-" + choiceRow.modelData.id
                    text: qsTr("Preview Left then Right")
                    Accessible.name: qsTr("Preview both for %1, Left then Right").arg(choiceRow.modelData.label)
                    onClicked: sourceChoiceList.chosen(choiceRow.modelData.id, "left-first")
                }
                Button {
                    objectName: "choicePreviewRightFirst-" + choiceRow.modelData.id
                    text: qsTr("Preview Right then Left")
                    Accessible.name: qsTr("Preview both for %1, Right then Left").arg(choiceRow.modelData.label)
                    onClicked: sourceChoiceList.chosen(choiceRow.modelData.id, "right-first")
                }
            }
            Label {
                visible: !choiceRow.modelData.available
                Layout.fillWidth: true
                text: choiceRow.modelData.detail
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
        }
        ScrollBar.vertical: ScrollBar {}
    }
    PlainInlineMessage {
        objectName: "choiceNotice"
        Layout.fillWidth: true
        onPlainTextRequested: item => sourceChoiceList.plainTextRequested(item)
        type: Kirigami.MessageType.Information
        visible: sourceChoiceList.noticeShown
        text: sourceChoiceList.selectedFile.choiceNotice || ""
    }
}
