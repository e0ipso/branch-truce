// The main window brings the session sidebar, comparison workspace and completion dialogs together.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window
    required property SessionModel files
    required property SessionLoader loader
    required property Selection selection
    required property SelectedFile selectedFile
    required property CommandFeedback feedback
    required property FileCommands commands
    required property ChoicePreviewFlow choices
    required property FileNavigator navigator
    required property ResultEditorAdapter editor
    required property SessionCompletion completion
    required property MergetoolStatus mergetool
    // Hiding the file list keeps the selected file, the editor location and the unsaved result.
    property bool fileListVisible: true
    readonly property bool supportedSelection: selectedFile.supported
    readonly property bool binarySelection: OutsideAppText.binaryExcluded(
        selectedFile.support === FileStates.SupportKind.OutsideApp, selectedFile.supportDetail, selectedFile.pathBytes)
    readonly property string selectedAddCommand: OutsideAppText.gitAddCommand(selectedFile.pathBytes)
    // Fills %1, %2 and so on in one pass, so repository text in a value is never
    // read as another placeholder.
    function fill(template: string, values: list<string>): string {
        return template.replace(/%(\d)/g, (match, index) => index > 0 && index <= values.length ? values[index - 1] : match);
    }
    function selectedProblemExplanation(): string {
        const path = selectedFile.path;
        const name = path.slice(path.lastIndexOf("/") + 1);
        const identity = name === path ? name : fill(qsTr("%1 (%2)"), [name, path]);
        if (selectedFile.support === FileStates.SupportKind.LargeFile)
            return qsTr("Are you sure you want to load it?");
        if (selectedFile.support === FileStates.SupportKind.Unreadable)
            return qsTr("Branch Truce could not read %1. Check that the file is available and readable before resolving it outside Branch Truce. It remains outstanding until Git reports it resolved.").arg(identity);
        const guidance = binarySelection
            ? qsTr("Resolve %1 with another tool.").arg(identity)
            : qsTr("Branch Truce cannot edit %1. Inspect the file and resolve it with another tool.").arg(identity);
        if (selectedAddCommand.length === 0)
            return qsTr("%1\n\nThis path cannot be represented safely in a copyable shell command. Stage the exact path with another tool after resolving it. It stays in this list and remains outstanding until Git reports it resolved.").arg(guidance);
        return fill(qsTr("%1 Then run this command from the repository root:\n%2\n\nIt stays in this list and remains outstanding until Git reports it resolved."),
            [guidance, selectedAddCommand]);
    }
    function choose(target: string, action: int): void {
        if (!choices.previewChoice(selectedFile.fileId, selectedFile.currentRevision, target, action))
            return;
        if (choices.choicePreview.confirmationRequired)
            replacementDialog.open();
        else
            choices.applyChoice(false);
    }
    function chooseNonConflicting(scope: int): void {
        if (choices.previewNonConflicting(selectedFile.fileId, selectedFile.currentRevision, scope))
            nonConflictingDialog.open();
    }
    function chooseWholeFile(side: int): void {
        if (choices.previewWholeFile(selectedFile.fileId, selectedFile.currentRevision, side))
            wholeFileDialog.open();
    }
    // Refused while an input method composes, so the composition keeps its place.
    function toggleFileList(): bool {
        if (comparison.inputMethodComposing)
            return false;
        editor.holdLocation();
        fileListVisible = !fileListVisible;
        editor.restoreLocation();
        return true;
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
    function retryPendingRecoveryAndClose(): void {
        if (completion.retryPendingRecovery())
            window.close();
    }
    // Closing the window is an ordinary finish action: with work outstanding it
    // opens the handoff instead, where leaving is a deliberate choice. Input that
    // recovery could not keep blocks the close until it is kept or the user cancels.
    onClosing: close => {
        if (window.files.hasPendingRecovery) {
            close.accepted = false;
            recoveryCloseDialog.open();
            return;
        }
        if (window.completion.outcome.length > 0 || window.files.totalFiles === 0
                || window.completion.handoff.clean)
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
        completion: window.completion
        onPlainTextRequested: item => window.showPlainText(item)
        rowDelegate: HandoffRow {
            selection: window.selection
            onRouted: handoffDialog.close()
            onDiscardStagedRequested: {
                handoffDialog.close();
                discardStagedDialog.ask(window.selectedFile.fileId, window.selectedFile.currentRevision);
            }
        }
    }
    DiscardStagedDialog {
        id: discardStagedDialog
        commands: window.commands
        feedback: window.feedback
        selectedFile: window.selectedFile
        onPlainTextRequested: item => window.showPlainText(item)
    }
    WholeFileDialog {
        id: wholeFileDialog
        choices: window.choices
        feedback: window.feedback
        composing: comparison.inputMethodComposing
    }
    NonConflictingDialog {
        id: nonConflictingDialog
        choices: window.choices
        feedback: window.feedback
        composing: comparison.inputMethodComposing
    }
    EditActions {
        id: resultEdits
        editor: window.editor
        commands: window.commands
        file: window.selectedFile
        composing: comparison.inputMethodComposing
    }
    ReplacementDialog {
        id: replacementDialog
        choices: window.choices
        feedback: window.feedback
        selectedFile: window.selectedFile
        composing: comparison.inputMethodComposing
    }
    Kirigami.Action {
        id: selectedDetailsAction
        objectName: "selectedProblemDetailsAction"
        text: qsTr("Technical details")
        icon.name: "documentinfo"
        enabled: window.selectedFile.supportDetail.length > 0
        onTriggered: selectedDetailsDialog.open()
    }
    // Loading may be slow, so a file over the line threshold waits for this.
    Kirigami.Action {
        id: loadLargeFileAction
        objectName: "loadLargeFileAction"
        text: qsTr("Load")
        icon.name: "document-open"
        onTriggered: window.commands.loadLargeFile(window.selectedFile.fileId)
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
    // Native grid units let the minimum follow the desktop font size.
    minimumWidth: Kirigami.Units.gridUnit * 48
    minimumHeight: Kirigami.Units.gridUnit * 28
    visible: true
    title: qsTr("Branch Truce")
    pageStack.initialPage: Kirigami.Page {
        padding: 0
        title: window.loader.operation || qsTr("Open conflict session")
        // Every attached tooltip uses one shared popup, and some name a path or a branch.
        Component.onCompleted: window.showPlainText(ToolTip.toolTip.contentItem)
        // The header names the repository and operation; progress stays in the footer.
        titleDelegate: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Theme.colorSet: Kirigami.Theme.Header
            ToolButton {
                objectName: "toggleFileList"
                text: window.fileListVisible ? qsTr("Hide file list") : qsTr("Show file list")
                Accessible.name: qsTr("Show file list")
                Accessible.role: Accessible.CheckBox
                icon.name: "sidebar-expand-left"
                display: AbstractButton.IconOnly
                checkable: true
                checked: window.fileListVisible
                onClicked: window.toggleFileList()
            }
            PlainLabel {
                objectName: "repositoryHeading"
                Layout.minimumWidth: Kirigami.Units.gridUnit * 5
                Layout.maximumWidth: Kirigami.Units.gridUnit * 12
                text: {
                    const path = window.loader.repositoryRoot
                    return path.length ? path.replace(/\/$/, "").split("/").pop() : qsTr("Open conflict session")
                }
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
                ToolTip.text: window.loader.repositoryRoot
                HoverHandler { id: repositoryHover }
                ToolTip.visible: repositoryHover.hovered && ToolTip.text.length > 0
            }
            PlainLabel {
                objectName: "repositorySubtitle"
                Layout.preferredWidth: Math.min(implicitWidth, Kirigami.Units.gridUnit * 12)
                Layout.minimumWidth: 0
                text: window.loader.operationDescription
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.disabledTextColor
                elide: Text.ElideRight
            }
            MainToolBar {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: !window.loader.opening && window.files.totalFiles > 0
                navigator: window.navigator
                selectedFile: window.selectedFile
                onHandoffRequested: handoffDialog.open()
            }
        }
        footer: SessionFooter {
            visible: !window.loader.opening && window.files.totalFiles > 0
            files: window.files
            loader: window.loader
            selectedFile: window.selectedFile
        }
        // Keep the panes' reading height when the work area needs vertical scrolling.
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
                // While the repository opens, and while the session has no files to show. A failed
                // or unsupported open names itself; it never reads as an empty session.
                CenteredPlaceholder {
                    visible: window.loader.opening || window.files.totalFiles === 0
                    messageName: "sessionPlaceholder"
                    onPlainTextRequested: item => window.showPlainText(item)
                    iconName: window.loader.opening ? "" : window.sessionPlaceholderIcon(window.loader.startupState)
                    text: window.loader.opening ? qsTr("Opening the repository")
                        : window.sessionPlaceholderText(window.loader.startupState)
                    explanation: window.loader.detail
                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: window.loader.opening
                        visible: running
                    }
                }
                // Several recovery sessions with app edits match: nothing is
                // restored until the user picks one here.
                ColumnLayout {
                    objectName: "recoveryChoices"
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: !window.loader.opening && window.loader.recoveryChoices.length > 0
                    PlainLabel {
                        Layout.fillWidth: true
                        text: qsTr("Choose the recovery session to restore. The other sessions are set aside, not deleted.")
                        wrapMode: Text.Wrap
                        font.bold: true
                    }
                    Repeater {
                        model: window.loader.recoveryChoices
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
                                onClicked: window.loader.chooseRecoverySession(recoveryChoice.modelData.session)
                            }
                        }
                    }
                }
                PlainInlineMessage {
                    objectName: "inventoryWarning"
                    onPlainTextRequested: item => window.showPlainText(item)
                    Accessible.name: qsTr("Session warning")
                    Layout.fillWidth: true
                    visible: !window.loader.opening
                        && window.files.outsideAppFiles > 0
                        && window.files.resolvedFiles + window.files.outsideAppFiles === window.files.totalFiles
                    type: Kirigami.MessageType.Warning
                    text: qsTr("Supported text work is complete. %n file(s) remain to handle outside the app.", "",
                        window.files.outsideAppFiles)
                }
                PlainLabel {
                    objectName: "navigationTarget"
                    Layout.fillWidth: true
                    visible: !window.loader.opening && (window.navigator.target.label || "").length > 0
                    text: window.navigator.target.label || ""
                    wrapMode: Text.Wrap
                }
                SplitView {
                    id: workSplit
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // A SplitView has no minimum of its own; the page scrolls once the work area reaches its minimum.
                    Layout.minimumHeight: workArea.Layout.minimumHeight
                    visible: !window.loader.opening && window.files.totalFiles > 0
                    FileList {
                        id: fileListPane
                        visible: window.fileListVisible
                        SplitView.preferredWidth: Kirigami.Units.gridUnit * 14
                            + Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing / 2
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                        SplitView.maximumWidth: Math.max(SplitView.minimumWidth, workSplit.width / 2)
                        files: window.files
                        selection: window.selection
                        headerHeight: fileToolBar.height
                        rowDelegate: FileRow {
                            id: fileRow
                            selection: window.selection
                            // A refused selection, as while an input method composes,
                            // leaves the list on the selected row.
                            onClicked: {
                                if (window.selection.select(fileRow.fileId))
                                    fileListPane.currentIndex = fileRow.index;
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
                            commands: window.commands
                            selectedFile: window.selectedFile
                            composing: comparison.inputMethodComposing
                            onSearchRequested: comparison.openSearch()
                            onNonConflictingChosen: scope => window.chooseNonConflicting(scope)
                            onWholeFileChosen: side => window.chooseWholeFile(side)
                            onDiscardToStagedChosen: discardStagedDialog.ask(window.selectedFile.fileId,
                                window.selectedFile.currentRevision)
                        }
                        // Explain the selected-file handoff; keep the exact diagnostic inspectable.
                        CenteredPlaceholder {
                            visible: window.selectedFile.fileId.length > 0 && !window.supportedSelection
                            messageName: "selectedProblem"
                            viewBackground: true
                            onPlainTextRequested: item => window.showPlainText(item)
                            iconName: window.selectedFile.support === FileStates.SupportKind.OutsideApp
                                ? window.selectedFile.mimeIcon : window.selectedFile.statusIcon
                            text: window.selectedFile.support === FileStates.SupportKind.LargeFile
                                ? qsTr("The file is over %1 lines").arg(window.files.largeFileLines)
                                : window.selectedFile.support === FileStates.SupportKind.Unreadable ? qsTr("Could not load this file")
                                : window.binarySelection ? qsTr("Binary files are resolved outside Branch Truce")
                                : qsTr("This file must be resolved outside Branch Truce")
                            explanation: window.selectedProblemExplanation()
                            helpfulAction: window.selectedFile.support === FileStates.SupportKind.LargeFile
                                ? loadLargeFileAction : selectedDetailsAction
                        }
                        Repeater {
                            model: FileNotices {
                                commands: window.commands
                                feedback: window.feedback
                                mergetool: window.mergetool
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
                            editorAdapter: window.editor
                            navigator: window.navigator
                            loader: window.loader
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
