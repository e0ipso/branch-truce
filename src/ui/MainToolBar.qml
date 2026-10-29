// The session toolbar: the handoff, difference navigation, the undecided stepper with its
// count, file navigation, and the file list toggle.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import org.kde.kirigami as Kirigami

Kirigami.ActionToolBar {
    id: mainToolBar
    required property ApplicationController controller
    signal handoffRequested()
    // The selected file's undecided choices; the stepper steps through them.
    readonly property int undecided: {
        const progress = controller.selectedFile.progress
        return progress ? Number(progress.undecided) : 0
    }
    objectName: "mainToolBar"
    actions: [
        NamedAction {
            controlName: "openHandoff"
            text: qsTr("Review handoff…")
            icon.name: "merge"
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: mainToolBar.handoffRequested()
        },
        NamedAction {
            controlName: "previousDifference"
            text: qsTr("Previous difference")
            icon.name: "go-previous"
            iconOnly: true
            onTriggered: mainToolBar.controller.navigate(FileNavigator.Difference, -1)
        },
        NamedAction {
            controlName: "nextDifference"
            text: qsTr("Next difference")
            icon.name: "go-next"
            iconOnly: true
            onTriggered: mainToolBar.controller.navigate(FileNavigator.Difference, 1)
        },
        NamedAction {
            controlName: "previousUndecided"
            text: qsTr("Previous undecided")
            icon.name: "arrow-up"
            iconOnly: true
            enabled: mainToolBar.undecided > 0
            onTriggered: mainToolBar.controller.navigate(FileNavigator.Undecided, -1)
        },
        // The count of undecided choices left in the selected file. Clicking it goes to the
        // next one, and it drops as choices are decided.
        NamedAction {
            controlName: "undecidedCounter"
            text: mainToolBar.undecided > 0 ? qsTr("%n undecided", "", mainToolBar.undecided)
                                            : qsTr("Nothing undecided")
            tooltip: mainToolBar.undecided > 0 ? qsTr("Go to the next undecided choice in this file")
                                               : qsTr("No undecided choices to step through")
            icon.name: "vcs-conflicting"
            displayHint: Kirigami.DisplayHint.KeepVisible
            enabled: mainToolBar.undecided > 0
            onTriggered: mainToolBar.controller.navigate(FileNavigator.Undecided, 1)
        },
        NamedAction {
            controlName: "nextUndecided"
            text: qsTr("Next undecided")
            icon.name: "arrow-down"
            iconOnly: true
            enabled: mainToolBar.undecided > 0
            onTriggered: mainToolBar.controller.navigate(FileNavigator.Undecided, 1)
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
                onTriggered: mainToolBar.controller.navigate(FileNavigator.Unfinished, -1)
            }
            MenuEntry {
                controlName: "nextUnfinished"
                text: qsTr("Next unfinished file")
                icon.name: "go-next-skip"
                onTriggered: mainToolBar.controller.navigate(FileNavigator.Unfinished, 1)
            }
            MenuEntry {
                controlName: "previousSavedOrReviewed"
                text: qsTr("Previous saved or reviewed")
                icon.name: "go-up"
                onTriggered: mainToolBar.controller.navigate(FileNavigator.SavedOrReviewed, -1)
            }
            MenuEntry {
                controlName: "nextSavedOrReviewed"
                text: qsTr("Next saved or reviewed")
                icon.name: "go-down"
                onTriggered: mainToolBar.controller.navigate(FileNavigator.SavedOrReviewed, 1)
            }
        },
        // Checked while the list shows; the name stays "Show file list" as the text changes.
        NamedAction {
            controlName: "toggleFileList"
            text: mainToolBar.controller.fileListVisible ? qsTr("Hide file list") : qsTr("Show file list")
            accessibleName: qsTr("Show file list")
            icon.name: "sidebar-expand-left"
            iconOnly: true
            displayHint: Kirigami.DisplayHint.KeepVisible
            toggle: true
            checked: mainToolBar.controller.fileListVisible
            onTriggered: mainToolBar.controller.toggleFileList()
        }
    ]
}
