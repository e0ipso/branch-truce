pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window
    required property var controller
    readonly property var selectedFile: controller.selectedFile
    readonly property bool supportedSelection: selectedFile.support === "supported"
    function choose(target: string, action: string): void {
        if (!controller.previewChoice(controller.selectedFileId, selectedFile.currentRevision, target, action))
            return;
        if (controller.commandPreview.confirmationRequired)
            replacementDialog.open();
        else
            controller.applyChoice(false);
    }
    function chooseNonConflicting(scope: string): void {
        if (controller.previewNonConflicting(controller.selectedFileId, selectedFile.currentRevision, scope))
            nonConflictingDialog.open();
    }
    function chooseWholeFile(side: string): void {
        if (controller.previewWholeFile(controller.selectedFileId, selectedFile.currentRevision, side))
            wholeFileDialog.open();
    }
    // One chip per progress fact: undecided count, save, review and resolution state.
    function progressChips(progress): var {
        if (!progress)
            return []
        const chips = [
            {icon: progress.undecided > 0 ? "dialog-question" : "checkmark",
             text: qsTr("Undecided: %1").arg(progress.undecided)},
            progress.saved ? {icon: "document-save", text: qsTr("Saved")}
                           : {icon: "vcs-locally-modified", text: qsTr("Not saved")},
            progress.reviewed ? {icon: "view-visible", text: qsTr("Reviewed")}
                              : {icon: "view-hidden", text: qsTr("Not reviewed")},
            progress.resolved ? {icon: "checkmark", text: qsTr("Resolved")}
                              : {icon: "vcs-conflicting", text: qsTr("Not resolved")}
        ]
        if (progress.earlierResolved)
            chips.push({icon: "vcs-update-required", text: qsTr("Earlier result staged; current draft not applied")})
        return chips
    }
    function progressSummary(progress): string {
        return progressChips(progress).map(chip => chip.text).join(" · ")
    }
    // The status sentence, the draft state, then the progress chips.
    function statusChips(status: string, support: string, unsaved: bool, progress): var {
        const chips = [{icon: "", text: status}]
        // Outside-app and unreadable files have no app draft to describe.
        if (support === "supported")
            chips.push(unsaved ? {icon: "document-edit", text: qsTr("Unsaved draft")}
                               : {icon: "document-save", text: qsTr("Manual Save checkpoint retained")})
        return chips.concat(progressChips(progress))
    }
    function statusLines(status: string, support: string, unsaved: bool, progress): string {
        const lines = statusChips(status, support, unsaved, null).map(chip => chip.text)
        lines.push(progressSummary(progress))
        return lines.join("\n")
    }
    // A source label with its commit id cut to 10 characters; the full id stays in the tooltip.
    function shortCommit(label: string, commitId: string): string {
        return commitId.length > 10 ? label.replace(commitId, commitId.slice(0, 10)) : label
    }
    // A file row shows the first line only: the outside-app reason or the undecided count,
    // otherwise the status. The rest stays in the text for the tooltip.
    function rowStatus(status: string, support: string, supportDetail: string, unsaved: bool, progress): string {
        let summary = status
        if (support !== "supported")
            summary = supportDetail || status
        else if (progress && progress.undecided > 0 && !progress.resolved)
            summary = qsTr("%1 undecided").arg(progress.undecided)
        const lines = statusLines(status, support, unsaved, progress).split("\n")
        return [summary].concat(lines.filter(line => line !== summary)).join("\n")
    }
    function rowIcon(support: string, unsaved: bool, progress): string {
        if (support === "unreadable")
            return "dialog-error"
        if (support !== "supported")
            return "unknown"
        if (progress && progress.resolved)
            return "checkmark"
        if (progress && progress.undecided > 0)
            return "vcs-conflicting"
        return unsaved ? "vcs-locally-modified" : "document-save"
    }
    function handoffIcon(category: string): string {
        switch (category) {
        case "published": return "checkmark"
        case "undecided": return "vcs-conflicting"
        case "saved-draft": return "document-save"
        case "newer-draft": return "vcs-update-required"
        case "written-unresolved": return "vcs-locally-modified"
        case "failed": return "dialog-error"
        case "outside-app": return "unknown"
        default: return "document-edit"
        }
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
            id: handoffRow
            controller: window.controller
            iconName: window.handoffIcon(handoffRow.modelData.category)
            onRouted: handoffDialog.close()
        }
    }
    WholeFileDialog {
        id: wholeFileDialog
        controller: window.controller
    }
    NonConflictingDialog {
        id: nonConflictingDialog
        controller: window.controller
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: !!window.selectedFile.canUndo && !!window.selectedFile.canEdit && !comparison.inputMethodComposing
        onActivated: window.controller.undo(window.controller.selectedFileId, window.selectedFile.currentRevision)
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: !!window.selectedFile.canRedo && !!window.selectedFile.canEdit && !comparison.inputMethodComposing
        onActivated: window.controller.redo(window.controller.selectedFileId, window.selectedFile.currentRevision)
    }
    ReplacementDialog {
        id: replacementDialog
        controller: window.controller
    }
    width: 1200
    height: 850
    // Fits 1920x1080 at 200% scaling (960x540 logical pixels); the page scrolls below its content height.
    minimumWidth: 900
    minimumHeight: 500
    visible: true
    title: qsTr("Branch Truce")
    pageStack.initialPage: Kirigami.Page {
        title: window.controller.operation || qsTr("Open conflict session")
        // Every attached tooltip uses one shared popup, and some name a path or a branch.
        Component.onCompleted: window.showPlainText(ToolTip.toolTip.contentItem)
        // The header names the repository once a root is known; the operation and the
        // file counts become its subtitle.
        titleDelegate: ColumnLayout {
            spacing: 0
            Layout.fillWidth: true
            Kirigami.Heading {
                objectName: "repositoryHeading"
                Layout.fillWidth: true
                level: 1
                text: window.controller.repositoryRoot || (window.controller.operation || qsTr("Open conflict session"))
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                HoverHandler { id: headingHover }
                ToolTip.text: window.controller.repositoryRoot
                ToolTip.visible: headingHover.hovered && truncated
                ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            Label {
                objectName: "repositorySubtitle"
                Layout.fillWidth: true
                visible: text.length > 0 && window.controller.repositoryRoot.length > 0
                text: {
                    const parts = [window.controller.operation]
                    if (!window.controller.opening && window.controller.files.totalFiles > 0)
                        parts.push(qsTr("%1 files in this session · %2 resolved")
                            .arg(window.controller.files.totalFiles).arg(window.controller.files.resolvedFiles))
                    return parts.filter(part => part.length > 0).join(" · ")
                }
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font: Kirigami.Theme.smallFont
                opacity: 0.7
            }
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
                spacing: Kirigami.Units.largeSpacing
                Label {
                    objectName: "sessionDetail"
                    Layout.fillWidth: true
                    visible: !window.controller.opening && window.controller.files.totalFiles > 0
                    text: window.controller.detail
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                // While the repository opens, and while the session has no files to show. A failed
                // or unsupported open names itself; it never reads as an empty session.
                CenteredPlaceholder {
                    visible: window.controller.opening || window.controller.files.totalFiles === 0
                    messageName: "sessionPlaceholder"
                    onPlainTextRequested: item => window.showPlainText(item)
                    iconName: window.controller.opening ? "" : window.sessionPlaceholderIcon(window.controller.startupState)
                    text: window.controller.opening ? qsTr("Opening the repository")
                        : window.sessionPlaceholderText(window.controller.startupState)
                    explanation: window.controller.detail
                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: window.controller.opening
                        visible: running
                    }
                }
                // Several recovery sessions with app edits match: nothing is
                // restored until the user picks one here.
                ColumnLayout {
                    objectName: "recoveryChoices"
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    visible: !window.controller.opening && window.controller.recoveryChoices.length > 0
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Choose the recovery session to restore. The other sessions are set aside, not deleted.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.bold: true
                    }
                    Repeater {
                        model: window.controller.recoveryChoices
                        delegate: RowLayout {
                            id: recoveryChoice
                            required property var modelData
                            Layout.fillWidth: true
                            Label {
                                objectName: "recoveryChoiceText-" + recoveryChoice.modelData.session
                                Layout.fillWidth: true
                                text: qsTr("Session %1, last changed %2").arg(recoveryChoice.modelData.session)
                                    .arg(recoveryChoice.modelData.lastChanged)
                                    + "\n" + qsTr("Edited files: %1").arg(recoveryChoice.modelData.paths.join(", "))
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            Button {
                                objectName: "recoveryChoice-" + recoveryChoice.modelData.session
                                text: qsTr("Restore this session")
                                onClicked: window.controller.chooseRecoverySession(recoveryChoice.modelData.session)
                            }
                        }
                    }
                }
                PlainInlineMessage {
                    objectName: "inventoryWarning"
                    onPlainTextRequested: item => window.showPlainText(item)
                    Accessible.name: qsTr("Session warning")
                    Layout.fillWidth: true
                    visible: !window.controller.opening
                        && window.controller.outsideAppFiles + window.controller.loadErrorFiles > 0
                    type: Kirigami.MessageType.Warning
                    text: qsTr("%1 outside app · %2 load errors. These files remain outstanding. Select a file for its reason and handle it outside Branch Truce.")
                        .arg(window.controller.outsideAppFiles).arg(window.controller.loadErrorFiles)
                }
                MainToolBar {
                    Layout.fillWidth: true
                    visible: !window.controller.opening && window.controller.files.totalFiles > 0
                    controller: window.controller
                    onHandoffRequested: handoffDialog.open()
                }
                Label {
                    objectName: "navigationTarget"
                    Layout.fillWidth: true
                    visible: !window.controller.opening && (window.controller.navigationTarget.label || "").length > 0
                    text: window.controller.navigationTarget.label || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                SplitView {
                    id: workSplit
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // A SplitView has no minimum of its own; the page scrolls once the work area reaches its minimum.
                    Layout.minimumHeight: workArea.Layout.minimumHeight
                    visible: !window.controller.opening && window.controller.files.totalFiles > 0
                    FileList {
                        id: fileListPane
                        visible: window.controller.fileListVisible
                        SplitView.preferredWidth: Kirigami.Units.gridUnit * 14
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                        SplitView.maximumWidth: Math.max(SplitView.minimumWidth, workSplit.width / 2)
                        controller: window.controller
                        rowDelegate: FileRow {
                            id: fileRow
                            controller: window.controller
                            iconName: window.rowIcon(fileRow.support, fileRow.unsaved, fileRow.progress)
                            statusText: window.rowStatus(fileRow.status, fileRow.support, fileRow.supportDetail, fileRow.unsaved,
                                fileRow.progress)
                            onClicked: {
                                fileListPane.currentIndex = fileRow.index;
                                window.controller.selectFile(fileRow.fileId);
                            }
                        }
                    }
                    ColumnLayout {
                        id: workArea
                        SplitView.fillWidth: true
                        // The selected file's name, its status sentence and one chip per state;
                        // read in order, the chip texts form the sidebar's status lines.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing
                            Label {
                                objectName: "selectedPath"
                                Layout.maximumWidth: workArea.width / 3
                                text: window.selectedFile.path || ""
                                textFormat: Text.PlainText
                                wrapMode: Text.WrapAnywhere
                                font.bold: true
                            }
                            Flow {
                                id: selectedStatus
                                objectName: "selectedStatus"
                                Accessible.role: Accessible.Grouping
                                Accessible.name: qsTr("File status")
                                Layout.fillWidth: true
                                visible: window.controller.selectedFileId.length > 0
                                spacing: Kirigami.Units.smallSpacing
                                Repeater {
                                    model: window.statusChips(window.selectedFile.status || "", window.selectedFile.support || "",
                                        !!window.selectedFile.unsaved, window.selectedFile.progress)
                                    delegate: StatusChip {
                                        maximumWidth: selectedStatus.width
                                    }
                                }
                            }
                        }
                        // The reason is shown whole; it stays outstanding until it is handled outside the app.
                        CenteredPlaceholder {
                            visible: window.controller.selectedFileId.length > 0 && !window.supportedSelection
                            messageName: "selectedProblem"
                            onPlainTextRequested: item => window.showPlainText(item)
                            iconName: window.rowIcon(window.selectedFile.support || "", false, null)
                            text: window.selectedFile.support === "unreadable" ? qsTr("Load error") : qsTr("Outside app")
                            explanation: (window.selectedFile.supportDetail || "") + "\n\n"
                                + qsTr("This file remains outstanding until it is handled outside Branch Truce.")
                            helpfulAction: Kirigami.Action {
                                text: qsTr("Review handoff…")
                                icon.name: "merge"
                                onTriggered: handoffDialog.open()
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
                        FileToolBar {
                            id: fileToolBar
                            visible: window.supportedSelection
                            controller: window.controller
                            selectedFile: window.selectedFile
                            composing: comparison.inputMethodComposing
                            onNonConflictingChosen: scope => window.chooseNonConflicting(scope)
                            onWholeFileChosen: side => window.chooseWholeFile(side)
                        }
                        ComparisonView {
                            id: comparison
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: window.supportedSelection
                            controller: window.controller
                            file: window.selectedFile
                            showMarkers: fileToolBar.showMarkers
                            onMarkersRevealed: fileToolBar.showMarkers = true
                        }
                        SourceChoiceList {
                            selectedFile: window.selectedFile
                            supportedSelection: window.supportedSelection
                            shown: fileToolBar.showSourceChoices
                            composing: comparison.inputMethodComposing
                            onPlainTextRequested: item => window.showPlainText(item)
                            onChosen: (target, action) => window.choose(target, action)
                        }
                    }
                }
                SessionFooter {
                    visible: !window.controller.opening && window.controller.files.totalFiles > 0
                    controller: window.controller
                }
            }
        }
    }
}
