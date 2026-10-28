// A Kirigami inline message that shows its text, and every label inside it, as plain text.
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.InlineMessage {
    // The text can carry a path, a label or Git output from the repository. Whoever creates
    // the message sets plain text on it and on each label Kirigami built inside it.
    signal plainTextRequested(item: Item)
    Component.onCompleted: plainTextRequested(this)
}
