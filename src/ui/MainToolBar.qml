// The session toolbar: the handoff, difference navigation, the conflict stepper with its
// count, file navigation, and the file list toggle.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: mainToolBar
    required property FileNavigator navigator
    required property SelectedFile selectedFile
    signal handoffRequested()
    // Current result regions, separate from the source decisions kept for review.
    readonly property int conflicts: {
        const progress = selectedFile.progress
        return progress ? Number(progress.conflicts) : 0
    }
    objectName: "mainToolBar"
    spacing: Kirigami.Units.smallSpacing
    Item { Layout.fillWidth: true }
    Frame {
        objectName: "conflictStepper"
        padding: 0
        contentItem: RowLayout {
            spacing: 0
            ToolButton {
                objectName: "previousUndecided"
                text: qsTr("Previous conflict")
                icon.name: "arrow-up"
                display: AbstractButton.IconOnly
                enabled: mainToolBar.conflicts > 0
                Accessible.name: text
                ToolTip.text: text
                ToolTip.visible: hovered
                ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: mainToolBar.navigator.navigate(FileNavigator.Conflict, -1)
            }
            Kirigami.Separator { Layout.fillHeight: true }
            ToolButton {
                objectName: "undecidedCounter"
                text: mainToolBar.conflicts > 0 ? qsTr("%n conflict(s) left", "", mainToolBar.conflicts)
                                                : qsTr("No conflicts to step through")
                icon.name: "vcs-conflicting"
                enabled: mainToolBar.conflicts > 0
                Accessible.name: text
                onClicked: mainToolBar.navigator.navigate(FileNavigator.Conflict, 1)
            }
            Kirigami.Separator { Layout.fillHeight: true }
            ToolButton {
                objectName: "nextUndecided"
                text: qsTr("Next conflict")
                icon.name: "arrow-down"
                display: AbstractButton.IconOnly
                enabled: mainToolBar.conflicts > 0
                Accessible.name: text
                ToolTip.text: text
                ToolTip.visible: hovered
                ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: mainToolBar.navigator.navigate(FileNavigator.Conflict, 1)
            }
        }
    }
    Kirigami.Separator { Layout.fillHeight: true }
    ToolButton {
        objectName: "previousUnfinished"
        text: qsTr("Previous file")
        icon.name: "go-previous-skip"
        Accessible.name: text
        onClicked: mainToolBar.navigator.navigate(FileNavigator.Unfinished, -1)
    }
    ToolButton {
        objectName: "nextUnfinished"
        text: qsTr("Next file")
        icon.name: "go-next-skip"
        Accessible.name: text
        onClicked: mainToolBar.navigator.navigate(FileNavigator.Unfinished, 1)
    }
    Kirigami.Separator { Layout.fillHeight: true }
    ToolButton {
        objectName: "openHandoff"
        text: qsTr("Review handoff…")
        icon.name: "documentinfo"
        Accessible.name: text
        onClicked: mainToolBar.handoffRequested()
    }
    readonly property MenuAction applicationMenu: MenuAction {
        controlName: "navigateMenu"
        text: qsTr("Navigate")
        tooltip: qsTr("Move between differences and saved or reviewed files")
        icon.name: "application-menu"
        iconOnly: true
        MenuEntry { controlName: "previousDifference"; text: qsTr("Previous difference"); icon.name: "go-previous"; onTriggered: mainToolBar.navigator.navigate(FileNavigator.Difference, -1) }
        MenuEntry { controlName: "nextDifference"; text: qsTr("Next difference"); icon.name: "go-next"; onTriggered: mainToolBar.navigator.navigate(FileNavigator.Difference, 1) }
        MenuEntry { controlName: "previousSavedOrReviewed"; text: qsTr("Previous saved or reviewed"); icon.name: "go-up"; onTriggered: mainToolBar.navigator.navigate(FileNavigator.SavedOrReviewed, -1) }
        MenuEntry { controlName: "nextSavedOrReviewed"; text: qsTr("Next saved or reviewed"); icon.name: "go-down"; onTriggered: mainToolBar.navigator.navigate(FileNavigator.SavedOrReviewed, 1) }
    }
    Loader { sourceComponent: mainToolBar.applicationMenu.displayComponent }
}
