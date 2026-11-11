// Session completion finishes a clean session or leaves one deliberately, and says what remains.
#pragma once
#include "SessionHandoff.h"
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace branch_truce {
class CommandFeedback;
class MergetoolSession;
class ResolutionPublisher;
class SessionStore;

class SessionCompletion : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    // Handoff::toVariant of handoff().
    Q_PROPERTY(QVariantMap handoff READ handoffData NOTIFY changed)
    // The final outcome once the session finished or was left, otherwise empty.
    Q_PROPERTY(QString outcome READ outcome NOTIFY changed)
public:
    SessionCompletion(SessionStore &store, const ResolutionPublisher &publisher, MergetoolSession &mergetool,
                      CommandFeedback &feedback, QObject *parent = nullptr);
    [[nodiscard]] Handoff handoff() const;
    [[nodiscard]] QVariantMap handoffData() const { return handoff().toVariant(); }
    [[nodiscard]] QString outcome() const { return outcome_; }
    [[nodiscard]] bool ended() const { return !outcome_.isEmpty(); }
    // Ends the session only when the handoff is clean.
    Q_INVOKABLE bool finish();
    // Deliberate leave: keeps drafts in the recovery store, tells waiting
    // launchers to leave their paths unresolved and runs no Git command.
    // Refused while pending input or a running resolution could be lost.
    Q_INVOKABLE bool leave();
    // Retries the pending FIFO asynchronously; recoveryRetried reports its outcome.
    Q_INVOKABLE bool retryPendingRecovery();

Q_SIGNALS:
    // The handoff or the outcome may have changed.
    void changed();
    void recoveryRetried(bool succeeded);
    // Emitted once by finish or leave; outcome is final.
    void sessionEnded(const QString &outcome, bool clean);

private:
    SessionStore &store_;
    const ResolutionPublisher &publisher_;
    MergetoolSession &mergetool_;
    CommandFeedback &feedback_;
    QString outcome_;
};
}
