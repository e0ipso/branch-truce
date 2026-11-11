// The mergetool status shows whether Git waits on the selected path and who stages it.
#pragma once
#include "publication/ResolutionPublisher.h"
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace branch_truce {
class MergetoolSession;
class Selection;
class SessionModel;

// The window's view of the waiting mergetool launchers and parent staging,
// for the selected file where it depends on one.
class MergetoolStatus : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the application")
    // The selected file's path while Git, not Branch Truce, stages it.
    Q_PROPERTY(QString parentStagingPath READ parentStagingPath NOTIFY changed)
    // Git was released to stage the selected path, and the index has not shown it after the
    // automatic checks ran out.
    Q_PROPERTY(bool parentStagingUnknown READ parentStagingUnknown NOTIFY changed)
    // Git launched the selected path and waits for its decision.
    Q_PROPERTY(bool waiting READ waiting NOTIFY changed)
    Q_PROPERTY(bool parentSessionActive READ parentSessionActive NOTIFY changed)
    Q_PROPERTY(bool canEndParentSession READ canEndParentSession NOTIFY changed)
public:
    MergetoolStatus(MergetoolSession &session, ResolutionPublisher &publisher, const SessionModel &files,
                    const Selection &selection, QObject *parent = nullptr);
    [[nodiscard]] QString parentStagingPath() const;
    [[nodiscard]] bool parentStagingUnknown() const;
    [[nodiscard]] bool waiting() const;
    [[nodiscard]] bool parentSessionActive() const;
    [[nodiscard]] bool canEndParentSession() const;
    // Tells the launcher waiting on the selected path to leave it unresolved.
    Q_INVOKABLE bool leave();
    Q_INVOKABLE bool endParentSession();
    // Checks again whether Git staged the selected file's offered revision.
    Q_INVOKABLE bool recheckParentAcknowledgment();

Q_SIGNALS:
    void changed();

private:
    MergetoolSession &session_;
    ResolutionPublisher &publisher_;
    const SessionModel &files_;
    const Selection &selection_;
};
}
