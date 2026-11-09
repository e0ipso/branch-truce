// The session footer shows resolved-file progress, outside-app work and captured file facts.
pragma ComponentBehavior: Bound
import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: sessionFooter
    required property SessionModel files
    required property SessionLoader loader
    required property SelectedFile selectedFile
    Layout.fillWidth: true
    implicitHeight: contents.implicitHeight
    Kirigami.Theme.colorSet: Kirigami.Theme.Header
    Kirigami.Theme.inherit: false
    color: Kirigami.Theme.backgroundColor
    ColumnLayout {
        id: contents
        anchors.fill: parent
        spacing: 0
        Kirigami.Separator { Layout.fillWidth: true }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.largeSpacing
            PlainLabel {
                id: workflowStatus
                objectName: "workflowStatus"
                text: qsTr("%1 of %n file(s) resolved", "", sessionFooter.files.totalFiles)
                    .arg(sessionFooter.files.resolvedFiles)
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                Accessible.description: sessionFooter.loader.workflow
                HoverHandler { id: progressHover }
                ToolTip.text: sessionFooter.loader.workflow
                ToolTip.visible: progressHover.hovered && ToolTip.text.length > 0
            }
            ProgressBar {
                visible: sessionFooter.width > Kirigami.Units.gridUnit * 40
                // The bar repeats the resolved count beside it, so it carries the same words.
                Accessible.name: workflowStatus.text
                Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                // Desktop-style insets otherwise make this the tallest footer item.
                implicitHeight: Kirigami.Units.smallSpacing
                topInset: 0
                bottomInset: 0
                from: 0; to: Math.max(1, sessionFooter.files.totalFiles)
                value: sessionFooter.files.resolvedFiles
            }
            Kirigami.Icon {
                visible: sessionFooter.files.outsideAppFiles > 0
                source: "dialog-warning"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            PlainLabel {
                objectName: "outsideAppCount"
                visible: sessionFooter.files.outsideAppFiles > 0
                text: qsTr("%n file(s) to handle outside the app", "", sessionFooter.files.outsideAppFiles)
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.neutralTextColor
            }
            Item { Layout.fillWidth: true }
            PlainLabel {
                objectName: "fileFacts"
                Layout.maximumWidth: sessionFooter.width / 3
                visible: sessionFooter.width > Kirigami.Units.gridUnit * 55
                text: {
                    // Launch facts describe the captured file, not unsaved buffer bytes.
                    const file = sessionFooter.selectedFile
                    const facts = [file.formatDescription, file.mimeDescription].filter(value => value.length > 0)
                    if (!file.supported) {
                        if (facts.length === 0)
                            facts.push(qsTr("Unknown type"))
                        const size = sessionFooter.loader.launchSizeText(file.fileId)
                        if (size.length > 0)
                            facts.push(size)
                    }
                    return facts.join(" · ")
                }
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
            }
        }
    }
}
