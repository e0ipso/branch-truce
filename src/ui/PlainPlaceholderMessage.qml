// A Kirigami placeholder message that shows its text, and every label inside it, as plain text.
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.PlaceholderMessage {
    // The text and explanation can carry a path or a reason from the repository. Whoever
    // creates the message sets plain text on it and on each label Kirigami built inside it.
    signal plainTextRequested(item: Item)
    Component.onCompleted: plainTextRequested(this)
}
