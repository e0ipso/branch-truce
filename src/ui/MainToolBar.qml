// The session toolbar: the handoff, difference and file navigation, and the file list toggle.
pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ActionToolBar {
    id: mainToolBar
    required property var controller
    signal handoffRequested()
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
            onTriggered: mainToolBar.controller.navigate("difference", -1)
        },
        NamedAction {
            controlName: "nextDifference"
            text: qsTr("Next difference")
            icon.name: "go-next"
            iconOnly: true
            onTriggered: mainToolBar.controller.navigate("difference", 1)
        },
        NamedAction {
            controlName: "previousUndecided"
            text: qsTr("Previous undecided")
            icon.name: "arrow-up"
            iconOnly: true
            onTriggered: mainToolBar.controller.navigate("undecided", -1)
        },
        NamedAction {
            controlName: "nextUndecided"
            text: qsTr("Next undecided")
            icon.name: "arrow-down"
            iconOnly: true
            onTriggered: mainToolBar.controller.navigate("undecided", 1)
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
                onTriggered: mainToolBar.controller.navigate("unfinished", -1)
            }
            MenuEntry {
                controlName: "nextUnfinished"
                text: qsTr("Next unfinished file")
                icon.name: "go-next-skip"
                onTriggered: mainToolBar.controller.navigate("unfinished", 1)
            }
            MenuEntry {
                controlName: "previousSavedOrReviewed"
                text: qsTr("Previous saved or reviewed")
                icon.name: "go-up"
                onTriggered: mainToolBar.controller.navigate("saved-or-reviewed", -1)
            }
            MenuEntry {
                controlName: "nextSavedOrReviewed"
                text: qsTr("Next saved or reviewed")
                icon.name: "go-down"
                onTriggered: mainToolBar.controller.navigate("saved-or-reviewed", 1)
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
