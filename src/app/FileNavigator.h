#pragma once
#include <QtQml/qqmlregistration.h>
#include "core/RevisionId.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace branch_truce {
class ResultEditorAdapter;
class SessionModel;

// Walks the selected file's differences or undecided regions in the result
// editor, or the session's unfinished or saved-or-reviewed files. A repeated
// step in the same mode continues from the last stop while the file, its
// revision and the editor selection are unchanged; otherwise the walk starts
// again from the cursor.
class FileNavigator : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ApplicationController")
public:
    enum class Mode { Difference, Undecided, Unfinished, SavedOrReviewed };
    Q_ENUM(Mode)
    // What the navigator needs from the window.
    struct Owners {
        std::function<QString()> selectedFileId;
        // The selected file's row in the session inventory, or -1.
        std::function<int()> selectedRow;
        // Selects a file stop; false when the window refuses the selection.
        std::function<bool(const QString &fileId)> selectFile;
        // Escaped display text for a file's exact path bytes.
        std::function<QString(const QString &fileId)> displayPath;
    };
    FileNavigator(SessionModel &files, ResultEditorAdapter &editor, Owners owners, QObject *parent = nullptr);

    // The last stop or failed step: mode, label and count, with the stop's
    // file, scope and range keys when there is one.
    [[nodiscard]] QVariantMap target() const { return target_; }
    // direction is -1 or 1. Refused while an input method is composing.
    bool navigate(Mode mode, int direction);
    // A file selection the navigator did not make ends the walk.
    void selectionChanged();
    void clear();

Q_SIGNALS:
    void changed();

private:
    bool navigateWithinFile(Mode mode, int direction);
    bool navigateAcrossFiles(Mode mode, int direction);

    SessionModel &files_;
    ResultEditorAdapter &editor_;
    Owners owners_;
    bool selecting_ = false;
    QVariantMap target_;
    std::optional<Mode> mode_;
    QString fileId_;
    RevisionId revision_ = 0;
    int index_ = -1;
    int cursor_ = -1;
    int selectionStart_ = -1;
    int selectionEnd_ = -1;
    std::size_t byteBegin_ = 0;
    std::size_t byteEnd_ = 0;
    std::string contribution_;
    bool resultRange_ = false;
};
}
