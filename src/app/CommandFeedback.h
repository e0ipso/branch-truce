// Command feedback is the one message the window shows about the last command, and the markers that refused it.
#pragma once
#include "core/RevisionId.h"
#include "core/Transition.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <cstddef>
#include <optional>
#include <string>

namespace branch_truce {
class SessionStore;
class Selection;

class CommandFeedback : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantMap markerBlocker READ markerBlocker NOTIFY changed)
public:
    CommandFeedback(const SessionStore &store, const Selection &selection, QObject *parent = nullptr);
    [[nodiscard]] QString message() const { return message_; }
    // Shows message and drops any marker blocker.
    void show(const QString &message);
    // Shows the sentence for a command's outcome, or clears the message when it
    // applied. True when the command applied or changed nothing.
    bool report(Transition result);
    // Shows message for Mark resolved refused by conflict markers in fileId at
    // revision: count markers, the first on the one-based line.
    void showMarkers(const QString &message, const std::string &fileId, RevisionId revision, std::size_t count,
                     std::size_t line);
    // The markers that refused Mark resolved, while the selected file is still
    // at that revision: fileId, revision, count and line. Empty otherwise.
    [[nodiscard]] QVariantMap markerBlocker() const;

Q_SIGNALS:
    // The message changed, or the marker blocker may have, since it follows
    // the selected file and its revision.
    void changed();

private:
    struct Markers {
        std::string fileId;
        RevisionId revision = 0;
        std::size_t count = 0;
        std::size_t line = 0;
    };
    const SessionStore &store_;
    const Selection &selection_;
    QString message_;
    std::optional<Markers> markers_;
};
}
