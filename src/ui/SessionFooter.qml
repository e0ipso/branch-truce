pragma ComponentBehavior: Bound
import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: sessionFooter
    required property ApplicationController controller
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
                objectName: "workflowStatus"
                text: (sessionFooter.controller.files.totalFiles === 1
                    ? qsTr("%1 of %n file resolved", "", sessionFooter.controller.files.totalFiles)
                    : qsTr("%1 of %n files resolved", "", sessionFooter.controller.files.totalFiles))
                    .arg(sessionFooter.controller.files.resolvedFiles)
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                Accessible.description: sessionFooter.controller.loader.workflow
                HoverHandler { id: progressHover }
                ToolTip.text: sessionFooter.controller.loader.workflow
                ToolTip.visible: progressHover.hovered && ToolTip.text.length > 0
            }
            ProgressBar {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                from: 0; to: Math.max(1, sessionFooter.controller.files.totalFiles)
                value: sessionFooter.controller.files.resolvedFiles
            }
            Kirigami.Icon {
                visible: sessionFooter.controller.outsideAppFiles > 0
                source: "dialog-warning"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            PlainLabel {
                objectName: "outsideAppCount"
                visible: sessionFooter.controller.outsideAppFiles > 0
                text: sessionFooter.controller.outsideAppFiles === 1 ? qsTr("%n file to handle outside the app", "", sessionFooter.controller.outsideAppFiles) : qsTr("%n files to handle outside the app", "", sessionFooter.controller.outsideAppFiles)
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
                    const file = sessionFooter.controller.selectedFile
                    const facts = [file.formatDescription, file.mimeDescription].filter(value => value.length > 0)
                    if (file.support !== "supported") {
                        if (facts.length === 0)
                            facts.push(qsTr("Unknown type"))
                        const size = sessionFooter.controller.loader.launchSizeText(file.fileId)
                        if (size.length > 0)
                            facts.push(size)
                    }
                    return facts.join(" · ")
                }
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
            }
            PlainLabel {
                objectName: "sourceCommits"
                visible: false
                text: sessionFooter.controller.loader.workflow
            }
        }
    }
}
