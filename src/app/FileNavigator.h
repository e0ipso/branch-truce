// File and conflict navigation keeps the selected file and result cursor on the requested work.
#pragma once
#include "core/RevisionId.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <cstddef>
#include <optional>
#include <string>

namespace branch_truce {
class ResultEditorAdapter;
class Selection;
class SessionModel;
class SessionStore;

// Walks the selected file's differences, conflicts or undecided choices in the result
// editor, or the session's unfinished or saved-or-reviewed files. A repeated
// step in the same mode continues from the last stop while the file, its
// revision and the editor selection are unchanged; otherwise the walk starts
// again from the cursor. A selection the navigator did not make ends the walk.
class FileNavigator : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(QVariantMap target READ target NOTIFY changed)
public:
    // Undecided retains contribution navigation; Conflict visits real result regions.
    enum class Mode { Difference, Undecided, Unfinished, SavedOrReviewed, Conflict };
    Q_ENUM(Mode)
    FileNavigator(const SessionStore &store, const SessionModel &files, ResultEditorAdapter &editor,
                  Selection &selection, QObject *parent = nullptr);

    // The last stop or failed step: mode, label and count, with the stop's
    // file, scope and range keys when there is one.
    [[nodiscard]] QVariantMap target() const { return target_; }
    // direction is -1 or 1. Difference, undecided and conflict stay inside the
    // selected file; the file modes walk the session inventory. Refused while
    // an input method is composing.
    Q_INVOKABLE bool navigate(branch_truce::FileNavigator::Mode mode, int direction);
    void clear();

Q_SIGNALS:
    void changed();

private:
    bool navigateWithinFile(Mode mode, int direction);
    bool navigateAcrossFiles(Mode mode, int direction);

    const SessionStore &store_;
    const SessionModel &files_;
    ResultEditorAdapter &editor_;
    Selection &selection_;
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
