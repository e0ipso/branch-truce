// An on-demand passage search with scope, match navigation and status.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: readingBar
    required property PassageSearch search
    readonly property string query: searchQuery.text
    readonly property bool caseSensitive: caseToggle.checked
    signal findRequested(bool forward)
    signal changed()
    objectName: "readingControls"
    spacing: Kirigami.Units.smallSpacing
    function open(): void {
        visible = true
        searchQuery.forceActiveFocus()
    }
    ComboBox {
        id: searchScope
        objectName: "searchScope"
        model: readingBar.search.scopeLabels
        currentIndex: readingBar.search.scopeIndex
        Accessible.name: qsTr("Search scope")
        Layout.preferredWidth: Math.max(implicitWidth, Kirigami.Units.gridUnit * 8)
        onCurrentIndexChanged: {
            readingBar.search.scopeIndex = currentIndex
            readingBar.changed()
        }
    }
    TextField {
        id: searchQuery
        objectName: "searchQuery"
        Layout.fillWidth: true
        Layout.minimumWidth: Kirigami.Units.gridUnit * 4
        placeholderText: qsTr("Find a passage")
        Accessible.name: qsTr("Passage search")
        onTextChanged: readingBar.changed()
        onAccepted: readingBar.findRequested(true)
    }
    ToolButton {
        objectName: "previousPassage"
        text: qsTr("Previous")
        icon.name: "go-up"
        display: AbstractButton.IconOnly
        onClicked: readingBar.findRequested(false)
    }
    ToolButton {
        objectName: "findPassage"
        text: qsTr("Next")
        icon.name: "go-down"
        display: AbstractButton.IconOnly
        onClicked: readingBar.findRequested(true)
    }
    ToolButton {
        id: caseToggle
        objectName: "searchCaseSensitive"
        text: qsTr("Match case")
        checkable: true
        onCheckedChanged: readingBar.changed()
    }
    PlainLabel {
        id: searchStatus
        objectName: "searchStatus"
        Layout.preferredWidth: Math.min(implicitWidth, Kirigami.Units.gridUnit * 12)
        text: readingBar.query.length > 0
            ? qsTr("%1 %2 of %3").arg(readingBar.search.status)
                .arg(readingBar.search.matchIndex).arg(readingBar.search.matchCount)
            : readingBar.search.status
        elide: Text.ElideRight
    }
    PlainLabel {
        visible: searchStatus.truncated && readingBar.query.length > 0
        text: qsTr("%1 of %2").arg(readingBar.search.matchIndex).arg(readingBar.search.matchCount)
    }
    ToolButton {
        objectName: "closePassageSearch"
        text: qsTr("Close search")
        icon.name: "window-close"
        display: AbstractButton.IconOnly
        onClicked: readingBar.visible = false
    }
    onVisibleChanged: changed()
}
