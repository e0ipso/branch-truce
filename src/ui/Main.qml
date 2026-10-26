pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigami.delegates as KirigamiDelegates

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
    // Closing the window is an ordinary finish action: with work outstanding it
    // opens the handoff instead, where leaving is a deliberate choice.
    onClosing: close => {
        if (window.controller.sessionOutcome.length > 0 || window.controller.files.totalFiles === 0
                || window.controller.handoff.clean)
            return;
        close.accepted = false;
        handoffDialog.open();
    }
    Dialog {
        id: handoffDialog
        objectName: "handoffDialog"
        readonly property var summary: window.controller.handoff
        title: qsTr("Session handoff")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 760)
        height: Math.min(window.height - 40, 640)
        standardButtons: Dialog.Close
        ScrollView {
            anchors.fill: parent
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: Kirigami.Units.smallSpacing
                Kirigami.InlineMessage {
                    objectName: "handoffOutcome"
                    Layout.fillWidth: true
                    visible: true
                    type: handoffDialog.summary.clean ? Kirigami.MessageType.Positive : Kirigami.MessageType.Warning
                    text: handoffDialog.summary.outcome || ""
                }
                Repeater {
                    model: handoffDialog.summary.sessionBlockers || []
                    delegate: Label {
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                }
                Repeater {
                    model: handoffDialog.summary.groups || []
                    delegate: ColumnLayout {
                        id: handoffGroup
                        required property var modelData
                        Layout.fillWidth: true
                        visible: modelData.entries.length > 0
                        Kirigami.Heading {
                            Layout.fillWidth: true
                            level: 4
                            text: qsTr("%1 (%2)").arg(handoffGroup.modelData.title).arg(handoffGroup.modelData.entries.length)
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Repeater {
                            model: handoffGroup.modelData.entries
                            delegate: RowLayout {
                                id: handoffEntry
                                required property var modelData
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: window.controller.displayPath(handoffEntry.modelData.fileId) + " · "
                                        + handoffEntry.modelData.status
                                        + (handoffEntry.modelData.blockers.length > 0
                                           ? "\n" + handoffEntry.modelData.blockers.join("; ") : "")
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                }
                                Button {
                                    objectName: "handoffRoute-" + handoffEntry.modelData.fileId
                                    text: handoffEntry.modelData.actionLabel || qsTr("Open file")
                                    onClicked: {
                                        if (window.controller.selectFile(handoffEntry.modelData.fileId))
                                            handoffDialog.close();
                                    }
                                }
                            }
                        }
                    }
                }
                Label {
                    objectName: "handoffLeaveBlocker"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: handoffDialog.summary.leaveBlocker || ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        objectName: "handoffFinish"
                        visible: !!handoffDialog.summary.clean
                        text: qsTr("Finish")
                        onClicked: window.controller.finishSession()
                    }
                    Button {
                        objectName: "handoffLeave"
                        visible: !handoffDialog.summary.clean
                        enabled: !!handoffDialog.summary.canLeave
                        text: qsTr("Leave with work outstanding")
                        onClicked: window.controller.leaveSession()
                    }
                }
            }
        }
    }
    Dialog {
        id: wholeFileDialog
        objectName: "wholeFileDialog"
        title: qsTr("Replace whole file from %1").arg(window.controller.wholeFilePreview.side || "")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 950)
        height: Math.min(window.height - 40, 650)
        standardButtons: Dialog.Apply | Dialog.Cancel
        onApplied: {
            window.controller.applyWholeFile();
            close();
        }
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.wholeFilePreview.path || ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.wholeFilePreview.source || ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
            }
            Label {
                objectName: "wholeFileWarning"
                Layout.fillWidth: true
                text: window.controller.wholeFilePreview.detail || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            TabBar {
                id: wholeFileTabs
                Layout.fillWidth: true
                TabButton { text: qsTr("Full file") }
                TabButton { text: qsTr("Contribution decisions") }
                TabButton { text: qsTr("Text and edits replaced") }
            }
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: wholeFileTabs.currentIndex
                RowLayout {
                    SnapshotPane {
                        paneName: "wholeFileBefore"
                        paneTitle: qsTr("Full current result, including manual edits")
                        paneText: window.controller.wholeFilePreview.before || ""
                    }
                    SnapshotPane {
                        paneName: "wholeFileAfter"
                        paneTitle: qsTr("Full proposed result")
                        paneText: window.controller.wholeFilePreview.replacement || ""
                    }
                }
                ColumnLayout {
                    Label {
                        Layout.fillWidth: true
                        visible: !!window.controller.wholeFilePreview.retainedDecisions
                        text: window.controller.wholeFilePreview.retainedDecisions || ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    ListView {
                        objectName: "wholeFileContributions"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: window.controller.wholeFilePreview.contributions || []
                        delegate: ColumnLayout {
                            id: wholeContribution
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: wholeContribution.modelData.scope + "\n"
                                    + wholeContribution.modelData.beforeState + " → " + wholeContribution.modelData.afterState
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            SnapshotPane {
                                Layout.preferredHeight: 120
                                paneName: "wholeContributionSource"
                                paneTitle: qsTr("Source contribution")
                                paneText: wholeContribution.modelData.sourceText
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
                ColumnLayout {
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("All differences from the selected source are replaced, including manual and pre-existing edits. Identical text stays the same.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    ListView {
                        objectName: "wholeFileDifferences"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: window.controller.wholeFilePreview.differences || []
                        delegate: ColumnLayout {
                            id: wholeDifference
                            required property var modelData
                            width: ListView.view.width
                            Label {
                                Layout.fillWidth: true
                                text: wholeDifference.modelData.scope
                                textFormat: Text.PlainText
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 150
                                SnapshotPane {
                                    paneName: "wholeDifferenceBefore"
                                    paneTitle: qsTr("Current text being replaced")
                                    paneText: wholeDifference.modelData.before
                                }
                                SnapshotPane {
                                    paneName: "wholeDifferenceAfter"
                                    paneTitle: qsTr("Replacement text")
                                    paneText: wholeDifference.modelData.replacement
                                }
                            }
                        }
                        ScrollBar.vertical: ScrollBar {}
                    }
                }
            }
        }
    }
    Dialog {
        id: nonConflictingDialog
        objectName: "nonConflictingDialog"
        title: qsTr("Apply non-conflicting changes · %1").arg(window.controller.nonConflictingPreview.scope || "")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 750)
        height: Math.min(window.height - 40, 600)
        standardButtons: Dialog.Apply | Dialog.Cancel
        onApplied: {
            window.controller.applyNonConflicting();
            close();
        }
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.nonConflictingPreview.path || ""
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.nonConflictingPreview.summary || ""
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: window.controller.nonConflictingPreview.detail || ""
                wrapMode: Text.Wrap
            }
            ListView {
                objectName: "nonConflictingChanges"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: window.controller.nonConflictingPreview.changes || []
                delegate: ColumnLayout {
                    id: batchChange
                    required property var modelData
                    width: ListView.view.width
                    Label {
                        Layout.fillWidth: true
                        text: batchChange.modelData.scope
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        SnapshotPane {
                            paneName: "batchBefore"
                            paneTitle: qsTr("Current text in this scope")
                            paneText: batchChange.modelData.before
                        }
                        SnapshotPane {
                            paneName: "batchAfter"
                            paneTitle: qsTr("Proposed text")
                            paneText: batchChange.modelData.replacement
                        }
                    }
                }
                ScrollBar.vertical: ScrollBar {}
            }
        }
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
    Dialog {
        id: replacementDialog
        objectName: "replacementDialog"
        title: window.controller.commandPreview.orderedCombination
            ? qsTr("Preview %1").arg(window.controller.commandPreview.order)
            : qsTr("Replace manual work in this scope?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(window.width - 40, 700)
        height: Math.min(window.height - 40, 600)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: window.controller.applyChoice(true)
        onRejected: window.controller.cancelChoice()
        ColumnLayout {
            anchors.fill: parent
            Label {
                Layout.fillWidth: true
                text: window.controller.commandPreview.detail || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: !!window.controller.commandPreview.orderedCombination
                text: window.controller.commandPreview.sourceScopes || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "manualReplacementNotice"
                Layout.fillWidth: true
                visible: !!window.controller.commandPreview.replacesManual
                text: qsTr("Manual or pre-existing edits in the current scope below will be replaced. Confirm to replace this work, or cancel to keep it.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                SnapshotPane {
                    paneName: "replacementBefore"
                    paneTitle: qsTr("Current text in this scope")
                    paneText: window.controller.commandPreview.before || ""
                }
                SnapshotPane {
                    paneName: "replacementAfter"
                    paneTitle: window.controller.commandPreview.orderedCombination
                        ? qsTr("Proposed text · %1").arg(window.controller.commandPreview.order)
                        : qsTr("Replacement from the selected source")
                    paneText: window.controller.commandPreview.replacement || ""
                }
            }
        }
    }
    component ConflictRow: KirigamiDelegates.SubtitleDelegate {
        id: row
        required property int index
        required property string fileId
        required property string support
        required property string supportDetail
        required property string status
        required property bool unsaved
        required property var progress
        readonly property bool current: fileId === window.controller.selectedFileId
        readonly property string chip: support === "unreadable" ? qsTr("Load error")
            : support === "supported" ? "" : qsTr("Outside app")
        width: ListView.view.width
        // The path alone names the row, so a screen reader reads it as the list item's name.
        text: window.controller.displayPath(fileId)
        subtitle: window.rowStatus(status, support, supportDetail, unsaved, progress)
        Accessible.description: subtitle
        icon.name: window.rowIcon(support, unsaved, progress)
        highlighted: current
        ToolTip.visible: hovered && (titleLabel.truncated || subtitleLabel.truncated)
        Keys.onReturnPressed: row.clicked()
        Keys.onEnterPressed: row.clicked()
        // One line each, so a path or a status never breaks inside a word.
        contentItem: RowLayout {
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                source: row.icon.name
                selected: row.highlighted
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    id: titleLabel
                    Layout.fillWidth: true
                    text: row.text
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.bold: row.current
                    color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                }
                // Holds every status line for the tooltip and shows only the first.
                Label {
                    id: subtitleLabel
                    Layout.fillWidth: true
                    text: row.subtitle
                    textFormat: Text.PlainText
                    maximumLineCount: 1
                    elide: Text.ElideRight
                    font: Kirigami.Theme.smallFont
                    color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                }
            }
            Label {
                Layout.alignment: Qt.AlignVCenter
                visible: row.chip.length > 0
                text: row.chip
                textFormat: Text.PlainText
                font: Kirigami.Theme.smallFont
                leftPadding: Kirigami.Units.smallSpacing
                rightPadding: Kirigami.Units.smallSpacing
                color: Kirigami.Theme.neutralTextColor
                background: Rectangle {
                    color: Kirigami.Theme.neutralBackgroundColor
                    radius: Kirigami.Units.cornerRadius
                }
            }
        }
        Kirigami.Separator {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: row.index < row.ListView.view.count - 1
        }
    }
    component SnapshotPane: ColumnLayout {
        id: pane
        required property string paneName
        required property string paneTitle
        required property string paneText
        Layout.fillWidth: true
        Layout.fillHeight: true
        Label {
            objectName: pane.paneName + "Label"
            Layout.fillWidth: true
            text: pane.paneTitle
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                objectName: pane.paneName + "Text"
                text: pane.paneText
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                font.family: "monospace"
                wrapMode: TextEdit.NoWrap
                Accessible.name: pane.paneTitle
            }
        }
    }
    // One status fact as an icon and its text. A chip without an icon is the status sentence
    // and has no background.
    component StatusChip: Rectangle {
        id: chip
        required property var modelData
        // A long status sentence wraps inside the chip instead of overflowing the row.
        property real maximumWidth: Infinity
        readonly property bool framed: (modelData.icon || "").length > 0
        readonly property real padding: framed ? Kirigami.Units.largeSpacing : 0
        implicitWidth: chipRow.implicitWidth + padding
        implicitHeight: chipRow.implicitHeight + (framed ? Kirigami.Units.smallSpacing : 0)
        radius: Kirigami.Units.cornerRadius
        color: framed ? Kirigami.Theme.alternateBackgroundColor : "transparent"
        RowLayout {
            id: chipRow
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                visible: chip.framed
                source: chip.modelData.icon || ""
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            Label {
                Layout.maximumWidth: chip.maximumWidth - chip.padding
                    - (chip.framed ? Kirigami.Units.iconSizes.small + chipRow.spacing : 0)
                text: chip.modelData.text || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }
        }
    }
    // A toolbar action whose button keeps an objectName, so tests find it by name and not
    // by its text. While the action fits, the toolbar shows this button instead of its own.
    // iconOnly hides the text here: Kirigami.DisplayHint.IconOnly would make the toolbar show
    // its own unnamed icon button instead.
    component NamedAction: Kirigami.Action {
        id: namedAction
        required property string controlName
        property string accessibleName: text
        property bool iconOnly: false
        displayComponent: ToolButton {
            objectName: namedAction.controlName
            action: namedAction
            display: namedAction.iconOnly ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
            Accessible.name: namedAction.accessibleName
            ToolTip.text: namedAction.tooltip || (display === AbstractButton.IconOnly ? namedAction.text : "")
            ToolTip.visible: hovered && ToolTip.text.length > 0
            ToolTip.delay: Kirigami.Units.toolTipDelay
        }
    }
    // An entry of a MenuAction, named like a toolbar button.
    component MenuEntry: Kirigami.Action {
        required property string controlName
    }
    // A toolbar button that opens a menu of its child entries. Each menu item takes its
    // entry's objectName, so a test opens the menu and clicks the entry by name.
    component MenuAction: NamedAction {
        id: menuAction
        displayComponent: ToolButton {
            id: menuButton
            objectName: menuAction.controlName
            action: menuAction
            down: pressed || entries.visible
            Accessible.role: Accessible.ButtonMenu
            Accessible.name: menuAction.accessibleName
            ToolTip.text: menuAction.tooltip
            ToolTip.visible: hovered && !entries.visible && ToolTip.text.length > 0
            ToolTip.delay: Kirigami.Units.toolTipDelay
            onClicked: entries.popup(menuButton, 0, menuButton.height)
            Menu {
                id: entries
                Instantiator {
                    model: menuAction.children
                    delegate: MenuItem {
                        required property MenuEntry modelData
                        objectName: modelData.controlName
                        action: modelData
                    }
                    onObjectAdded: (index, entry) => entries.insertItem(index, entry as MenuItem)
                    onObjectRemoved: (index, entry) => entries.removeItem(entry as MenuItem)
                }
            }
        }
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
                RowLayout {
                    BusyIndicator { running: window.controller.opening; visible: running }
                    Label {
                        Layout.fillWidth: true
                        text: window.controller.detail
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
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
                Kirigami.InlineMessage {
                    objectName: "inventoryWarning"
                    Accessible.name: qsTr("Session warning")
                    Layout.fillWidth: true
                    visible: !window.controller.opening
                        && window.controller.outsideAppFiles + window.controller.loadErrorFiles > 0
                    type: Kirigami.MessageType.Warning
                    text: qsTr("%1 outside app · %2 load errors. These files remain outstanding. Select a file for its reason and handle it outside Branch Truce.")
                        .arg(window.controller.outsideAppFiles).arg(window.controller.loadErrorFiles)
                }
                Kirigami.ActionToolBar {
                    objectName: "mainToolBar"
                    Layout.fillWidth: true
                    visible: !window.controller.opening && window.controller.files.totalFiles > 0
                    actions: [
                        NamedAction {
                            controlName: "openHandoff"
                            text: qsTr("Review handoff…")
                            icon.name: "merge"
                            displayHint: Kirigami.DisplayHint.KeepVisible
                            onTriggered: handoffDialog.open()
                        },
                        NamedAction {
                            controlName: "previousDifference"
                            text: qsTr("Previous difference")
                            icon.name: "go-previous"
                            iconOnly: true
                            onTriggered: window.controller.navigate("difference", -1)
                        },
                        NamedAction {
                            controlName: "nextDifference"
                            text: qsTr("Next difference")
                            icon.name: "go-next"
                            iconOnly: true
                            onTriggered: window.controller.navigate("difference", 1)
                        },
                        NamedAction {
                            controlName: "previousUndecided"
                            text: qsTr("Previous undecided")
                            icon.name: "arrow-up"
                            iconOnly: true
                            onTriggered: window.controller.navigate("undecided", -1)
                        },
                        NamedAction {
                            controlName: "nextUndecided"
                            text: qsTr("Next undecided")
                            icon.name: "arrow-down"
                            iconOnly: true
                            onTriggered: window.controller.navigate("undecided", 1)
                        },
                        MenuAction {
                            controlName: "navigateMenu"
                            text: qsTr("Navigate")
                            tooltip: qsTr("Move between unfinished, saved or reviewed files")
                            icon.name: "go-jump"
                            MenuEntry {
                                controlName: "previousUnfinished"
                                text: qsTr("Previous unfinished file")
                                icon.name: "go-previous-skip"
                                onTriggered: window.controller.navigate("unfinished", -1)
                            }
                            MenuEntry {
                                controlName: "nextUnfinished"
                                text: qsTr("Next unfinished file")
                                icon.name: "go-next-skip"
                                onTriggered: window.controller.navigate("unfinished", 1)
                            }
                            MenuEntry {
                                controlName: "previousSavedOrReviewed"
                                text: qsTr("Previous saved or reviewed")
                                icon.name: "go-up"
                                onTriggered: window.controller.navigate("saved-or-reviewed", -1)
                            }
                            MenuEntry {
                                controlName: "nextSavedOrReviewed"
                                text: qsTr("Next saved or reviewed")
                                icon.name: "go-down"
                                onTriggered: window.controller.navigate("saved-or-reviewed", 1)
                            }
                        },
                        // Checked while the list shows; the name stays "Show file list" as the text changes.
                        NamedAction {
                            controlName: "toggleFileList"
                            text: window.controller.fileListVisible ? qsTr("Hide file list") : qsTr("Show file list")
                            accessibleName: qsTr("Show file list")
                            icon.name: "sidebar-expand-left"
                            iconOnly: true
                            displayHint: Kirigami.DisplayHint.KeepVisible
                            checked: window.controller.fileListVisible
                            onTriggered: window.controller.toggleFileList()
                        }
                    ]
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
                    Rectangle {
                        visible: window.controller.fileListVisible
                        SplitView.preferredWidth: Kirigami.Units.gridUnit * 14
                        SplitView.minimumWidth: Kirigami.Units.gridUnit * 8
                        SplitView.maximumWidth: Math.max(SplitView.minimumWidth, workSplit.width / 2)
                        Kirigami.Theme.colorSet: Kirigami.Theme.View
                        Kirigami.Theme.inherit: false
                        color: Kirigami.Theme.backgroundColor
                        ListView {
                            id: fileList
                            objectName: "fileList"
                            Accessible.name: qsTr("File list")
                            anchors.fill: parent
                            activeFocusOnTab: true
                            keyNavigationEnabled: true
                            clip: true
                            model: window.controller.files
                            currentIndex: 0
                            function revealSelection(): void {
                                const row = window.controller.selectedRow()
                                if (row < 0 || row >= count)
                                    return
                                currentIndex = row
                                positionViewAtIndex(row, ListView.Contain)
                            }
                            Component.onCompleted: revealSelection()
                            Connections {
                                target: window.controller
                                function onSelectedFileChanged(): void {
                                    fileList.revealSelection()
                                }
                            }
                            delegate: ConflictRow {
                                id: fileDelegate
                                onClicked: {
                                    fileList.currentIndex = fileDelegate.index;
                                    window.controller.selectFile(fileDelegate.fileId);
                                }
                            }
                            ScrollBar.vertical: ScrollBar {}
                        }
                        Kirigami.PlaceholderMessage {
                            anchors.centerIn: parent
                            width: parent.width - Kirigami.Units.gridUnit * 2
                            visible: fileList.count === 0
                            icon.name: "checkmark"
                            text: qsTr("No conflicted files")
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
                        Label {
                            Layout.fillWidth: true
                            objectName: "selectedProblem"
                            visible: window.controller.selectedFileId.length > 0 && !window.supportedSelection
                            text: (window.selectedFile.support === "unreadable"
                                ? qsTr("Load error · outside app · outstanding\n") : qsTr("Outside app · outstanding\n"))
                                + (window.selectedFile.supportDetail || "")
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }
                        Kirigami.InlineMessage {
                            objectName: "recoveryProblem"
                            Layout.fillWidth: true
                            visible: !!window.selectedFile.recoveryError
                            type: Kirigami.MessageType.Error
                            text: window.selectedFile.recoveryError || ""
                        }
                        Kirigami.InlineMessage {
                            objectName: "commandMessage"
                            type: Kirigami.MessageType.Information
                            icon.name: "dialog-information"
                            Layout.fillWidth: true
                            visible: window.controller.commandMessage.length > 0
                            text: window.controller.commandMessage
                        }
                        Kirigami.InlineMessage {
                            objectName: "parentStagingState"
                            type: Kirigami.MessageType.Warning
                            icon.name: "vcs-locally-modified"
                            Layout.fillWidth: true
                            visible: window.controller.parentStagingPath.length > 0
                            text: qsTr("Written, unresolved. Waiting for Git to stage %1.").arg(window.controller.parentStagingPath)
                        }
                        Button {
                            objectName: "recheckParentStaging"
                            visible: window.controller.parentStagingPath.length > 0
                            text: qsTr("Check Git staging")
                            enabled: !window.controller.resolutionBusy
                            onClicked: window.controller.recheckParentAcknowledgment()
                        }
                        Kirigami.InlineMessage {
                            objectName: "mergetoolHolding"
                            type: Kirigami.MessageType.Information
                            icon.name: "vcs-conflicting"
                            Layout.fillWidth: true
                            visible: window.controller.mergetoolWaiting
                            text: qsTr("Git is waiting on this path. Mark resolved hands it back. Leave keeps the draft and returns the original file.")
                        }
                        Button {
                            objectName: "leaveMergetool"
                            visible: window.controller.mergetoolWaiting
                            enabled: !window.controller.resolutionBusy
                            text: qsTr("Leave unresolved")
                            onClicked: window.controller.leaveMergetool()
                        }
                        // A path whose result Git already staged is not waiting for a launch.
                        Kirigami.InlineMessage {
                            objectName: "mergetoolWait"
                            type: Kirigami.MessageType.Warning
                            icon.name: "chronometer"
                            Layout.fillWidth: true
                            visible: window.controller.parentSessionActive && !window.controller.mergetoolWaiting
                                     && window.controller.parentStagingPath.length === 0 && window.supportedSelection
                                     && !window.selectedFile.resolved
                                     && !(window.selectedFile.progress && window.selectedFile.progress.earlierResolved)
                            text: qsTr("Waiting for Git to launch this path. It is not resolved.")
                        }
                        Button {
                            objectName: "endParentSession"
                            visible: window.controller.canEndParentSession
                            text: qsTr("Parent Git is finished")
                            onClicked: window.controller.endParentSession()
                        }
                        RowLayout {
                            visible: window.controller.resolutionConfirmation
                                && window.controller.resolutionFileId === window.controller.selectedFileId
                                && window.controller.resolutionRevision === window.selectedFile.currentRevision
                            Button {
                                objectName: "confirmResolution"
                                text: qsTr("Confirm resolution")
                                enabled: !window.controller.resolutionBusy && !comparison.inputMethodComposing
                                onClicked: window.controller.markResolved(window.controller.selectedFileId, window.selectedFile.currentRevision, true)
                            }
                            Button {
                                objectName: "cancelResolution"
                                text: qsTr("Cancel")
                                onClicked: window.controller.cancelResolution()
                            }
                        }
                        Button {
                            objectName: "retryRecovery"
                            visible: !!window.selectedFile.pendingInput && window.selectedFile.recoveryState !== "save-failed"
                            text: qsTr("Retry keeping pending input")
                            onClicked: window.controller.files.retryPendingRecovery(window.controller.selectedFileId)
                        }
                        Button {
                            objectName: "retrySave"
                            visible: window.selectedFile.recoveryState === "save-failed"
                            text: qsTr("Retry Save")
                            onClicked: window.controller.files.retryPendingRecovery(window.controller.selectedFileId)
                        }
                        SnapshotPane {
                            visible: !!window.selectedFile.pendingInput && window.selectedFile.recoveryState !== "save-failed"
                            paneName: "pendingInput"
                            paneTitle: qsTr("Pending input · not acknowledged")
                            paneText: window.selectedFile.pendingText || ""
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: window.supportedSelection
                            Kirigami.ActionToolBar {
                                objectName: "fileToolBar"
                                Layout.fillWidth: true
                                actions: [
                                    NamedAction {
                                        controlName: "saveDraft"
                                        text: qsTr("Save")
                                        icon.name: "document-save"
                                        displayHint: Kirigami.DisplayHint.KeepVisible
                                        enabled: !!window.selectedFile.canEdit && !!window.selectedFile.unsaved && !comparison.inputMethodComposing
                                        onTriggered: window.controller.saveDraft(window.controller.selectedFileId, window.selectedFile.currentRevision)
                                    },
                                    NamedAction {
                                        controlName: "markResolved"
                                        text: qsTr("Mark resolved")
                                        icon.name: "dialog-ok-apply"
                                        displayHint: Kirigami.DisplayHint.KeepVisible
                                        enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing && !window.controller.resolutionBusy
                                        onTriggered: window.controller.markResolved(window.controller.selectedFileId, window.selectedFile.currentRevision, false)
                                    },
                                    NamedAction {
                                        controlName: "discardUnsaved"
                                        text: qsTr("Discard")
                                        tooltip: qsTr("Discard unsaved edits")
                                        icon.name: "document-revert"
                                        enabled: !!window.selectedFile.canEdit && !!window.selectedFile.unsaved && !comparison.inputMethodComposing
                                        onTriggered: window.controller.discardUnsaved(window.controller.selectedFileId, window.selectedFile.currentRevision)
                                    },
                                    NamedAction {
                                        controlName: "undoCommand"
                                        text: qsTr("Undo")
                                        tooltip: qsTr("Undo the last edit or choice. Typing and choices share one history.")
                                        icon.name: "edit-undo"
                                        iconOnly: true
                                        enabled: !!window.selectedFile.canEdit && !!window.selectedFile.canUndo && !comparison.inputMethodComposing
                                        onTriggered: window.controller.undo(window.controller.selectedFileId, window.selectedFile.currentRevision)
                                    },
                                    NamedAction {
                                        controlName: "redoCommand"
                                        text: qsTr("Redo")
                                        tooltip: qsTr("Redo the last undone edit or choice. Typing and choices share one history.")
                                        icon.name: "edit-redo"
                                        iconOnly: true
                                        enabled: !!window.selectedFile.canEdit && !!window.selectedFile.canRedo && !comparison.inputMethodComposing
                                        onTriggered: window.controller.redo(window.controller.selectedFileId, window.selectedFile.currentRevision)
                                    },
                                    NamedAction {
                                        controlName: "copyResult"
                                        text: qsTr("Copy")
                                        icon.name: "edit-copy"
                                        iconOnly: true
                                        onTriggered: window.controller.resultClipboard("copy")
                                    },
                                    NamedAction {
                                        controlName: "pasteResult"
                                        text: qsTr("Paste")
                                        icon.name: "edit-paste"
                                        iconOnly: true
                                        enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing
                                        onTriggered: window.controller.resultClipboard("paste")
                                    },
                                    MenuAction {
                                        id: fileChoices
                                        controlName: "sourceChoicesMenu"
                                        text: qsTr("Source choices")
                                        tooltip: qsTr("Preview non-conflicting changes or a whole side of %1 before applying")
                                            .arg(window.selectedFile.path || "")
                                        icon.name: "document-import"
                                        enabled: !!window.selectedFile.canEdit && !comparison.inputMethodComposing
                                        MenuEntry {
                                            controlName: "nonConflictingLeft"
                                            text: qsTr("Preview non-conflicting Left")
                                            icon.name: "arrow-left"
                                            enabled: fileChoices.enabled
                                            onTriggered: window.chooseNonConflicting("left")
                                        }
                                        MenuEntry {
                                            controlName: "nonConflictingRight"
                                            text: qsTr("Preview non-conflicting Right")
                                            icon.name: "arrow-right"
                                            enabled: fileChoices.enabled
                                            onTriggered: window.chooseNonConflicting("right")
                                        }
                                        MenuEntry {
                                            controlName: "nonConflictingBoth"
                                            text: qsTr("Preview non-conflicting Both")
                                            icon.name: "merge"
                                            enabled: fileChoices.enabled
                                            onTriggered: window.chooseNonConflicting("both")
                                        }
                                        MenuEntry {
                                            controlName: "wholeFileLeft"
                                            text: qsTr("Preview whole file from Left")
                                            icon.name: "arrow-left-double"
                                            enabled: fileChoices.enabled
                                            onTriggered: window.chooseWholeFile("left")
                                        }
                                        MenuEntry {
                                            controlName: "wholeFileRight"
                                            text: qsTr("Preview whole file from Right")
                                            icon.name: "arrow-right-double"
                                            enabled: fileChoices.enabled
                                            onTriggered: window.chooseWholeFile("right")
                                        }
                                    },
                                    NamedAction {
                                        id: sourceChoices
                                        controlName: "showSourceChoices"
                                        text: checked ? qsTr("Hide source choices") : qsTr("Show source choices")
                                        icon.name: "view-list-details"
                                        iconOnly: true
                                        checkable: true
                                    }
                                ]
                            }
                            Label {
                                visible: comparison.inputMethodComposing
                                text: qsTr("Composing input · not acknowledged")
                                textFormat: Text.PlainText
                            }
                        }
                        ComparisonView {
                            id: comparison
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: window.supportedSelection
                            controller: window.controller
                            file: window.selectedFile
                        }
                        ListView {
                            objectName: "scopedChoices"
                            visible: window.supportedSelection && sourceChoices.checked
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(contentHeight, 180)
                            clip: true
                            model: window.selectedFile.choices || []
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
                                    enabled: !!window.selectedFile.canEdit && !!choiceRow.modelData.available && !comparison.inputMethodComposing
                                    Button {
                                        objectName: "choiceAccept-" + choiceRow.modelData.id
                                        text: qsTr("Accept →")
                                        Accessible.name: qsTr("Accept %1").arg(choiceRow.modelData.label)
                                        onClicked: window.choose(choiceRow.modelData.id, "accept")
                                    }
                                    Button {
                                        objectName: "choiceIgnore-" + choiceRow.modelData.id
                                        text: qsTr("× Ignore")
                                        Accessible.name: qsTr("Ignore %1").arg(choiceRow.modelData.label)
                                        onClicked: window.choose(choiceRow.modelData.id, "ignore")
                                    }
                                    Button {
                                        objectName: "choiceResolve-" + choiceRow.modelData.id
                                        text: qsTr("Resolve using this side")
                                        Accessible.name: qsTr("Resolve %1 using this side and ignore its competitor").arg(choiceRow.modelData.label)
                                        onClicked: window.choose(choiceRow.modelData.id, "resolve")
                                    }
                                }
                                RowLayout {
                                    enabled: !!window.selectedFile.canEdit && !!choiceRow.modelData.combinationAvailable && !comparison.inputMethodComposing
                                    Button {
                                        objectName: "choicePreviewLeftFirst-" + choiceRow.modelData.id
                                        text: qsTr("Preview Left then Right")
                                        Accessible.name: qsTr("Preview both for %1, Left then Right").arg(choiceRow.modelData.label)
                                        onClicked: window.choose(choiceRow.modelData.id, "left-first")
                                    }
                                    Button {
                                        objectName: "choicePreviewRightFirst-" + choiceRow.modelData.id
                                        text: qsTr("Preview Right then Left")
                                        Accessible.name: qsTr("Preview both for %1, Right then Left").arg(choiceRow.modelData.label)
                                        onClicked: window.choose(choiceRow.modelData.id, "right-first")
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
                        Kirigami.InlineMessage {
                            objectName: "choiceNotice"
                            Layout.fillWidth: true
                            type: Kirigami.MessageType.Information
                            visible: sourceChoices.checked && !!window.selectedFile.choiceNotice
                            text: window.selectedFile.choiceNotice || ""
                        }

                    }
                }
                // The footer is one line once files load: the two source commits, shortened,
                // and where the work continues.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.largeSpacing
                    visible: !window.controller.opening && window.controller.files.totalFiles > 0
                    Label {
                        objectName: "sourceCommits"
                        visible: window.controller.stage2.length > 0
                        text: window.shortCommit(window.controller.stage2, window.controller.stage2CommitId) + " · "
                            + window.shortCommit(window.controller.stage3, window.controller.stage3CommitId)
                        textFormat: Text.PlainText
                        HoverHandler { id: sourceCommitsHover }
                        ToolTip.text: window.controller.stage2 + "\n" + window.controller.stage3
                        ToolTip.visible: sourceCommitsHover.hovered
                        ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                    Label {
                        objectName: "workflowStatus"
                        Layout.fillWidth: true
                        text: window.controller.workflow
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        HoverHandler { id: workflowHover }
                        ToolTip.text: text
                        ToolTip.visible: workflowHover.hovered && truncated
                        ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }
            }
        }
    }
}
