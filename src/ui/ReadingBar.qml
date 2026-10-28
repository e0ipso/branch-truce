// The reading controls above the panes: passage search and its scope, line wrapping,
// whitespace markers and the search status.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: readingBar
    required property string statusText
    // The view fills in the scopes, since the earlier version adds one while it is shown.
    property alias scope: searchScope
    readonly property string query: searchQuery.text
    readonly property bool wrapLines: wrapLinesBox.checked
    readonly property bool showWhitespace: showWhitespaceBox.checked
    signal scopeSwitched()
    signal findRequested()
    signal whitespaceToggled()
    objectName: "readingControls"
    spacing: Kirigami.Units.smallSpacing
    RowLayout {
        Layout.fillWidth: true
        ComboBox {
            id: searchScope
            objectName: "searchScope"
            model: [qsTr("Left source"), qsTr("Editable result"), qsTr("Right source")]
            Accessible.name: qsTr("Search scope")
            Layout.preferredWidth: Math.max(implicitWidth, Kirigami.Units.gridUnit * 10)
            onCurrentIndexChanged: readingBar.scopeSwitched()
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
            onToggled: readingBar.whitespaceToggled()
        }
        Label {
            objectName: "searchStatus"
            Layout.fillWidth: true
            text: readingBar.statusText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }
}
