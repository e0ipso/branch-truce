pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window
    required property ApplicationController controller
    readonly property SelectedFile selectedFile: controller.selectedFile
    readonly property bool supportedSelection: selectedFile.support === "supported"
    readonly property bool binarySelection: OutsideAppText.binaryExcluded(
        selectedFile.support, selectedFile.supportDetail, selectedFile.pathBytes)
    readonly property string selectedAddCommand: OutsideAppText.gitAddCommand(selectedFile.pathBytes)
    function selectedProblemExplanation(): string {
        const path = selectedFile.path;
        const name = path.slice(path.lastIndexOf("/") + 1);
        const identity = name === path ? name : name + " (" + path + ")";
        if (selectedFile.support === "unreadable")
            return qsTr("Branch Truce could not read %1. Check that the file is available and readable before resolving it outside Branch Truce. It remains outstanding until Git reports it resolved.").arg(identity);
        const guidance = binarySelection
            ? qsTr("Resolve %1 with another tool.").arg(identity)
            : qsTr("Branch Truce cannot edit %1. Inspect the file and resolve it with another tool.").arg(identity);
        if (selectedAddCommand.length === 0)
            return qsTr("%1\n\nThis path cannot be represented safely in a copyable shell command. Stage the exact path with another tool after resolving it. It stays in this list and remains outstanding until Git reports it resolved.").arg(guidance);
        // Substitute repository text only once per string, so a path containing %1 or
        // %2 is never interpreted as another translation placeholder.
        return guidance + qsTr(" Then run this command from the repository root:\n%1\n\nIt stays in this list and remains outstanding until Git reports it resolved.")
            .arg(selectedAddCommand);
    }
    function choose(target: string, action: int): void {
        if (!controller.previewChoice(controller.selectedFileId, selectedFile.currentRevision, target, action))
            return;
        if (controller.commandPreview.confirmationRequired)
            replacementDialog.open();
        else
            controller.applyChoice(false);
    }
    function chooseNonConflicting(scope: int): void {
        if (controller.previewNonConflicting(controller.selectedFileId, selectedFile.currentRevision, scope))
            nonConflictingDialog.open();
    }
    function chooseWholeFile(side: int): void {
        if (controller.previewWholeFile(controller.selectedFileId, selectedFile.currentRevision, side))
            wholeFileDialog.open();
    }
    // The placeholder for a session with nothing to list: an empty session, an unsupported
    // operation and a failed open are three different messages.
    function sessionPlaceholderIcon(state: string): string {
        switch (state) {
        case "error": return "dialog-error"
        case "unsupported": return "unknown"
        default: return "checkmark"
        }
    }
    function sessionPlaceholderText(state: string): string {
        switch (state) {
        case "error": return qsTr("Could not open the conflicts")
        case "unsupported": return qsTr("This operation is outside Branch Truce")
        default: return qsTr("No conflicted files")
        }
    }
    // Kirigami notices and the shared tooltip detect rich text, and their text can carry a path,
    // a label or Git output from the repository. They offer no text format, so set plain text on
    // the item and every label inside it: markup then shows as typed and loads nothing.
    function showPlainText(item: Item): void {
        const pending = [item];
        while (pending.length > 0) {
            const next = pending.pop();
            if (next["textFormat"] !== undefined)
                next["textFormat"] = Text.PlainText;
            pending.push(...next.children);
        }
    }
    // Retries every file's pending recovery, then closes again if none is left.
    function retryPendingRecoveryAndClose(): void {
        for (const group of controller.handoff.groups || [])
            for (const entry of group.entries)
                controller.files.retryPendingRecovery(entry.fileId);
        if (!controller.files.hasPendingRecovery)
            window.close();
    }
    // Closing the window is an ordinary finish action: with work outstanding it
    // opens the handoff instead, where leaving is a deliberate choice. Input that
    // recovery could not keep blocks the close until it is kept or the user cancels.
    onClosing: close => {
        if (window.controller.files.hasPendingRecovery) {
            close.accepted = false;
            recoveryCloseDialog.open();
            return;
        }
        if (window.controller.sessionOutcome.length > 0 || window.controller.files.totalFiles === 0
                || window.controller.handoff.clean)
            return;
        close.accepted = false;
        handoffDialog.open();
    }
    Kirigami.PromptDialog {
        id: recoveryCloseDialog
        objectName: "recoveryCloseDialog"
        title: qsTr("Pending input needs recovery")
        subtitle: qsTr("Recovery could not keep the pending input. Retry before closing, or cancel to keep this window open.")
        dialogType: Kirigami.PromptDialog.Warning
        standardButtons: Kirigami.Dialog.Retry | Kirigami.Dialog.Cancel
        onAccepted: window.retryPendingRecoveryAndClose()
    }
    HandoffDialog {
        id: handoffDialog
        controller: window.controller
        onPlainTextRequested: item => window.showPlainText(item)
        rowDelegate: HandoffRow {
            controller: window.controller
            onRouted: handoffDialog.close()
            onDiscardStagedRequested: {
                handoffDialog.close();
                discardStagedDialog.ask(window.controller.selectedFileId, window.selectedFile.currentRevision);
            }
        }
    }
    DiscardStagedDialog {
        id: discardStagedDialog
        controller: window.controller
        onPlainTextRequested: item => window.showPlainText(item)
    }
    WholeFileDialog {
        id: wholeFileDialog
        controller: window.controller
    }
    NonConflictingDialog {
        id: nonConflictingDialog
        controller: window.controller
    }
    EditActions {
        id: resultEdits
        controller: window.controller
        file: window.selectedFile
        composing: comparison.inputMethodComposing
    }
    ReplacementDialog {
        id: replacementDialog
        controller: window.controller
    }
    Kirigami.Dialog {
        id: selectedDetailsDialog
        objectName: "selectedProblemDetails"
        title: qsTr("File diagnostics")
        standardButtons: Kirigami.Dialog.Close
        preferredWidth: Kirigami.Units.gridUnit * 30
        DialogBody {
            PlainLabel {
                objectName: "selectedProblemDiagnosticPath"
                Layout.fillWidth: true
                text: window.selectedFile.path
                font.bold: true
                wrapMode: Text.WrapAnywhere
            }
            TextArea {
                objectName: "selectedProblemDiagnostic"
                Layout.fillWidth: true
                text: window.selectedFile.supportDetail
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                Accessible.name: qsTr("Captured file diagnostic")
            }
        }
    }
    width: Kirigami.Units.gridUnit * 70
    height: Kirigami.Units.gridUnit * 40
    // Fits 1920x1080 at 200% scaling (960x540 logical pixels); the page scrolls below its content height.
    minimumWidth: Kirigami.Units.gridUnit * 48
    minimumHeight: Kirigami.Units.gridUnit * 28
    visible: true
    title: qsTr("Branch Truce")
    pageStack.initialPage: Kirigami.Page {
        padding: 0
        title: window.controller.loader.operation || qsTr("Open conflict session")
        // Every attached tooltip uses one shared popup, and some name a path or a branch.
        Component.onCompleted: window.showPlainText(ToolTip.toolTip.contentItem)
        // The header names the repository and operation; progress stays in the footer.
        titleDelegate: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Theme.colorSet: Kirigami.Theme.Header
            ToolButton {
                objectName: "toggleFileList"
                text: window.controller.fileListVisible ? qsTr("Hide file list") : qsTr("Show file list")
                Accessible.name: qsTr("Show file list")
                Accessible.role: Accessible.CheckBox
                icon.name: "sidebar-expand-left"
                display: AbstractButton.IconOnly
                checkable: true
                checked: window.controller.fileListVisible
                onClicked: window.controller.toggleFileList()
            }
            PlainLabel {
                objectName: "repositoryHeading"
                Layout.minimumWidth: Kirigami.Units.gridUnit * 5
                Layout.maximumWidth: Kirigami.Units.gridUnit * 12
                text: {
                    const path = window.controller.loader.repositoryRoot
                    return path.length ? path.replace(/\/$/, "").split("/").pop() : qsTr("Open conflict session")
                }
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
                ToolTip.text: window.controller.loader.repositoryRoot
                HoverHandler { id: repositoryHover }
                ToolTip.visible: repositoryHover.hovered && ToolTip.text.length > 0
            }
            PlainLabel {
                objectName: "repositorySubtitle"
                Layout.preferredWidth: Math.min(implicitWidth, Kirigami.Units.gridUnit * 12)
                Layout.minimumWidth: 0
                text: window.controller.loader.operationDescription
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
            }
            MainToolBar {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: !window.controller.loader.opening && window.controller.files.totalFiles > 0
                controller: window.controller
                onHandoffRequested: handoffDialog.open()
            }
        }
        footer: SessionFooter {
            visible: !window.controller.loader.opening && window.controller.files.totalFiles > 0
            controller: window.controller
        }
        // On a short screen, such as 1080p at 150% or 200%, the content keeps its minimum
        // height and scrolls, so the text panes keep a usable height and nothing is cut off.
        Flickable {
            id: pageScroll
            objectName: "pageScroll"
            readonly property bool overflowing: contentHeight > height
            anchors.fill: parent
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, pageContent.Layout.minimumHeight)
            boundsBehavior: Flickable.StopAtBounds
            // Mouse drags select text in the panes; the wheel and the scroll bar move the page.
            interactive: false
            ScrollBar.vertical: ScrollBar {
                id: pageScrollBar
                policy: pageScroll.overflowing ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            }
            Kirigami.WheelHandler {
                target: pageScroll
            }
            ColumnLayout {
                id: pageContent
                width: pageScroll.width - (pageScroll.overflowing ? pageScrollBar.width : 0)
                height: pageScroll.contentHeight
                spacing: 0
                PlainLabel {
                    objectName: "sessionDetail"
                    Layout.fillWidth: true
                    visible: false
                    text: window.controller.loader.detail
                    wrapMode: Text.Wrap
                }
                // While the repository opens, and while the session has no files to show. A failed
                // or unsupported open names itself; it never reads as an empty session.
                CenteredPlaceholder {
                    visible: window.controller.loader.opening || window.controller.files.totalFiles === 0
                    messageName: "sessionPlaceholder"
                    onPlainTextRequested: item => window.showPlainText(item)
                    iconName: window.controller.loader.opening ? "" : window.sessionPlaceholderIcon(window.controller.loader.startupState)
                    text: window.controller.loader.opening ? qsTr("Opening the repository")
                        : window.sessionPlaceholderText(window.controller.loader.startupState)
                    explanation: window.controller.loader.detail
                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: window.controller.loader.opening
                        visible: running
                    }
                }
                // Several recovery sessions with app edits match: nothing is
                // restored until the user picks one here.
                ColumnLayout {
                    objectName: "recoveryChoices"
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: !window.controller.loader.opening && window.controller.loader.recoveryChoices.length > 0
                    PlainLabel {
                        Layout.fillWidth: true
                        text: qsTr("Choose the recovery session to restore. The other sessions are set aside, not deleted.")
                        wrapMode: Text.Wrap
                        font.bold: true
                    }
                    Repeater {
                        model: window.controller.loader.recoveryChoices
                        delegate: RowLayout {
                            id: recoveryChoice
                            required property var modelData
                            Layout.fillWidth: true
                            PlainLabel {
                                objectName: "recoveryChoiceText-" + recoveryChoice.modelData.session
                                Layout.fillWidth: true
                                text: qsTr("Session %1, last changed %2\nEdited files: %3")
                                    .arg(recoveryChoice.modelData.session).arg(recoveryChoice.modelData.lastChanged)
                                    .arg(recoveryChoice.modelData.paths.join(qsTr(", ")))
                                wrapMode: Text.Wrap
                            }
                            Button {
                                objectName: "recoveryChoice-" + recoveryChoice.modelData.session
                                text: qsTr("Restore this session")
                                onClicked: window.controller.loader.chooseRecoverySession(recoveryChoice.modelData.session)
                            }
                        }
                    }
                }
                PlainInlineMessage {
                    objectName: "inventoryWarning"
                    onPlainTextRequested: item => window.showPlainText(item)
                    Accessible.name: qsTr("Session warning")
                    Layout.fillWidth: true
                    visible: !window.controller.loader.opening
                        && window.controller.outsideAppFiles > 0
                        && window.controller.files.resolvedFiles + window.controller.outsideAppFiles === window.controller.files.totalFiles
                    type: Kirigami.MessageType.Warning
                    text: window.controller.outsideAppFiles === 1 ? qsTr("Supported text work is complete. %n file remains to handle outside the app.", "", window.controller.outsideAppFiles) : qsTr("Supported text work is complete. %n files remain to handle outside the app.", "", window.controller.outsideAppFiles)
                }
                PlainLabel {
                    objectName: "navigationTarget"
                    Layout.fillWidth: true
                    visible: !window.controller.loader.opening && (window.controller.navigationTarget.label || "").length > 0
                    text: window.controller.navigationTarget.label || ""
                    wrapMode: Text.Wrap
                }
                SplitView {
                    id: workSplit
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // A SplitView has no minimum of its own; the page scrolls once the work area reaches its minimum.
                    Layout.minimumHeight: workArea.Layout.minimumHeight
                    visible: !window.controller.loader.opening && window.controller.files.totalFiles > 0
                    FileList {
                        id: fileListPane
                        visible: window.controller.fileListVisible
                        SplitView.preferredWidth: Kirigami.Units.gridUnit * 14
                            + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing / 2
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                        SplitView.maximumWidth: Math.max(SplitView.minimumWidth, workSplit.width / 2)
                        controller: window.controller
                        headerHeight: fileToolBar.height
                        rowDelegate: FileRow {
                            id: fileRow
                            controller: window.controller
                            onClicked: {
                                fileListPane.currentIndex = fileRow.index;
                                window.controller.selectFile(fileRow.fileId);
                            }
                        }
                    }
                    ColumnLayout {
                        id: workArea
                        SplitView.fillWidth: true
                        spacing: 0
                        FileToolBar {
                            id: fileToolBar
                            edits: resultEdits
                            controller: window.controller
                            selectedFile: window.selectedFile
                            composing: comparison.inputMethodComposing
                            onSearchRequested: comparison.openSearch()
                            onNonConflictingChosen: scope => window.chooseNonConflicting(scope)
                            onWholeFileChosen: side => window.chooseWholeFile(side)
                            onDiscardToStagedChosen: discardStagedDialog.ask(window.controller.selectedFileId,
                                window.selectedFile.currentRevision)
                        }
                        Item {
                            objectName: "selectedStatus"
                            visible: false
                        }
                        // Explain the selected-file handoff; keep the exact diagnostic inspectable.
                        CenteredPlaceholder {
                            visible: window.controller.selectedFileId.length > 0 && !window.supportedSelection
                            messageName: "selectedProblem"
                            viewBackground: true
                            onPlainTextRequested: item => window.showPlainText(item)
                            iconName: window.selectedFile.support === "outside-app"
                                ? window.selectedFile.mimeIcon : window.selectedFile.statusIcon
                            text: window.selectedFile.support === "unreadable" ? qsTr("Could not load this file")
                                : window.binarySelection ? qsTr("Binary files are resolved outside Branch Truce")
                                : qsTr("This file must be resolved outside Branch Truce")
                            explanation: window.selectedProblemExplanation()
                            helpfulAction: Kirigami.Action {
                                objectName: "selectedProblemDetailsAction"
                                text: qsTr("Technical details")
                                icon.name: "documentinfo"
                                enabled: window.selectedFile.supportDetail.length > 0
                                onTriggered: selectedDetailsDialog.open()
                            }
                        }
                        Repeater {
                            model: FileNotices {
                                controller: window.controller
                                selectedFile: window.selectedFile
                                supportedSelection: window.supportedSelection
                                composing: comparison.inputMethodComposing
                                onPlainTextRequested: item => window.showPlainText(item)
                                onFirstMarkerShown: {
                                    comparison.revealCursorLine()
                                    comparison.editorNamed("result").forceActiveFocus()
                                }
                            }
                        }
                        // Why the region choices are missing from the gutters, while they are shown.
                        PlainInlineMessage {
                            objectName: "choiceNotice"
                            Layout.fillWidth: true
                            onPlainTextRequested: item => window.showPlainText(item)
                            type: Kirigami.MessageType.Information
                            visible: window.supportedSelection && fileToolBar.showSourceChoices
                                && !!window.selectedFile.choiceNotice
                            text: window.selectedFile.choiceNotice || ""
                        }
                        ComparisonView {
                            id: comparison
                            edits: resultEdits
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: window.supportedSelection
                            controller: window.controller
                            file: window.selectedFile
                            showMarkers: fileToolBar.showMarkers
                            wrapLines: fileToolBar.wrapLines
                            showWhitespace: fileToolBar.showWhitespace
                            showEarlier: fileToolBar.showEarlier
                            showChoices: fileToolBar.showSourceChoices
                            onMarkersRevealed: fileToolBar.showMarkers = true
                            onChoiceChosen: (target, action) => window.choose(target, action)
                        }
                    }
                }
            }
        }
    }
}
