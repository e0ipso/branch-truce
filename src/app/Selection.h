// The selection names the one file the window shows and every command acts on.
#pragma once
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <string>

namespace branch_truce {
class SessionModel;

// What may keep the selection where it is, such as an input method composing
// in the result editor.
class SelectionGuard {
public:
    virtual ~SelectionGuard() = default;
    [[nodiscard]] virtual bool holdsSelection() const = 0;
};

// The selected file, by its exact byte-derived ID, never by a display path.
// When the selected file leaves the session, the preferred file or the first
// listed file takes its place.
class Selection : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(QString fileId READ fileId NOTIFY changed)
public:
    Selection(const SessionModel &files, const SelectionGuard &guard, QObject *parent = nullptr);
    [[nodiscard]] QString fileId() const { return fileId_; }
    [[nodiscard]] std::string id() const { return fileId_.toStdString(); }
    // The selected file's row in the list, or -1.
    Q_INVOKABLE int row() const;
    // Refused for a file outside the session and while the guard holds the
    // selection. Selecting the selected file again succeeds and changes nothing.
    Q_INVOKABLE bool select(const QString &fileId);
    // The file to select once the session lists it, such as the path a
    // mergetool launcher opened the window on.
    void prefer(const QString &fileId) { preferred_ = fileId; }

Q_SIGNALS:
    // The selected file ID changed.
    void changed();

private:
    void filesChanged();

    const SessionModel &files_;
    const SelectionGuard &guard_;
    QString fileId_;
    QString preferred_;
};
}
