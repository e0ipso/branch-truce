// The reading controls above the panes: passage search and its scope, line wrapping,
// whitespace markers and the search status.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: readingBar
    // The scopes, the selected scope and the status come from the search.
    required property PassageSearch search
    readonly property string query: searchQuery.text
    readonly property bool wrapLines: wrapLinesBox.checked
    readonly property bool showWhitespace: showWhitespaceBox.checked
    signal findRequested()
    objectName: "readingControls"
    spacing: Kirigami.Units.smallSpacing
    RowLayout {
        Layout.fillWidth: true
        ComboBox {
            id: searchScope
            objectName: "searchScope"
            model: readingBar.search.scopeLabels
            currentIndex: readingBar.search.scopeIndex
            Accessible.name: qsTr("Search scope")
            Layout.preferredWidth: Math.max(implicitWidth, Kirigami.Units.gridUnit * 10)
            onCurrentIndexChanged: readingBar.search.scopeIndex = currentIndex
        }
        TextField {
            id: searchQuery
            objectName: "searchQuery"
            Layout.fillWidth: true
            placeholderText: qsTr("Find a passage")
            Accessible.name: qsTr("Passage search")
            onAccepted: readingBar.findRequested()
        }
        Button {
            objectName: "findPassage"
            text: qsTr("Find")
            onClicked: readingBar.findRequested()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        CheckBox {
            id: wrapLinesBox
            objectName: "wrapLines"
            text: qsTr("Wrap lines")
        }
        CheckBox {
            id: showWhitespaceBox
            objectName: "showWhitespace"
            text: qsTr("Show spaces and tabs")
        }
        Label {
            objectName: "searchStatus"
            Layout.fillWidth: true
            text: readingBar.search.status
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }
}
