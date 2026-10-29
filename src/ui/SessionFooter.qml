// The footer, one line once files load: the two source commits, shortened, and where the
// work continues.
pragma ComponentBehavior: Bound

import QtQuick
import BranchTruce.App
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: sessionFooter
    required property ApplicationController controller
    Layout.fillWidth: true
    spacing: Kirigami.Units.largeSpacing
    PlainLabel {
        objectName: "sourceCommits"
        Layout.maximumWidth: sessionFooter.width / 2
        visible: sessionFooter.controller.loader.stage2.length > 0
        text: qsTr("%1: %2 · %3: %4").arg(sessionFooter.controller.loader.stage2)
            .arg(sessionFooter.controller.loader.stage2ShortCommitId).arg(sessionFooter.controller.loader.stage3)
            .arg(sessionFooter.controller.loader.stage3ShortCommitId)
    }
    PlainLabel {
        objectName: "workflowStatus"
        Layout.fillWidth: true
        text: sessionFooter.controller.loader.workflow
        elide: Text.ElideRight
    }
}
