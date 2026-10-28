// The footer, one line once files load: the two source commits, shortened, and where the
// work continues.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

RowLayout {
    id: sessionFooter
    required property var controller
    // A source label with its commit id cut to 10 characters; the full id stays in the tooltip.
    function shortCommit(label: string, commitId: string): string {
        return commitId.length > 10 ? label.replace(commitId, commitId.slice(0, 10)) : label
    }
    Layout.fillWidth: true
    spacing: Kirigami.Units.largeSpacing
    Label {
        objectName: "sourceCommits"
        visible: sessionFooter.controller.loader.stage2.length > 0
        text: sessionFooter.shortCommit(sessionFooter.controller.loader.stage2, sessionFooter.controller.loader.stage2CommitId) + " · "
            + sessionFooter.shortCommit(sessionFooter.controller.loader.stage3, sessionFooter.controller.loader.stage3CommitId)
        textFormat: Text.PlainText
        HoverHandler { id: sourceCommitsHover }
        ToolTip.text: sessionFooter.controller.loader.stage2 + "\n" + sessionFooter.controller.loader.stage3
        ToolTip.visible: sourceCommitsHover.hovered
        ToolTip.delay: Kirigami.Units.toolTipDelay
    }
    Label {
        objectName: "workflowStatus"
        Layout.fillWidth: true
        text: sessionFooter.controller.loader.workflow
        textFormat: Text.PlainText
        elide: Text.ElideRight
        HoverHandler { id: workflowHover }
        ToolTip.text: text
        ToolTip.visible: workflowHover.hovered && truncated
        ToolTip.delay: Kirigami.Units.toolTipDelay
    }
}
