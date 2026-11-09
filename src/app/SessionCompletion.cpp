// Session completion finishes a clean session or leaves one deliberately, and says what remains.
#include "SessionCompletion.h"
#include "CommandFeedback.h"
#include "mergetool/MergetoolSession.h"
#include "publication/ResolutionPublisher.h"
#include "session/SessionStore.h"

namespace branch_truce {
SessionCompletion::SessionCompletion(SessionStore &store, const ResolutionPublisher &publisher,
                                     MergetoolSession &mergetool, CommandFeedback &feedback, QObject *parent)
    : QObject(parent), store_(store), publisher_(publisher), mergetool_(mergetool), feedback_(feedback)
{
    // The handoff reads the files, the running resolution and the launches Git still waits on.
    connect(&store_, &SessionStore::fileChanged, this, &SessionCompletion::changed);
    connect(&store_, &SessionStore::replaced, this, &SessionCompletion::changed);
    connect(&store_, &SessionStore::appended, this, &SessionCompletion::changed);
    connect(&publisher_, &ResolutionPublisher::changed, this, &SessionCompletion::changed);
    connect(&mergetool_, &MergetoolSession::changed, this, &SessionCompletion::changed);
}

Handoff SessionCompletion::handoff() const
{
    return SessionHandoff::build(store_, {publisher_.busy(), mergetool_.unreleasedLaunches(), ended()});
}

bool SessionCompletion::finish()
{
    if (ended()) return false;
    const auto summary = handoff();
    if (!summary.clean) {
        feedback_.show(tr("Work remains outstanding, so this session cannot finish cleanly. "
                          "Open an outstanding file or leave deliberately."));
        return false;
    }
    outcome_ = summary.outcome;
    Q_EMIT changed();
    Q_EMIT sessionEnded(outcome_, true);
    return true;
}

bool SessionCompletion::leave()
{
    const auto summary = handoff();
    if (!summary.canLeave) {
        if (!summary.leaveBlocker.isEmpty()) feedback_.show(summary.leaveBlocker);
        return false;
    }
    const auto launches = mergetool_.unreleasedLaunches();
    if (launches > 0) (void)mergetool_.leaveAll();
    if (summary.clean)
        outcome_ = summary.outcome;
    else if (launches > 0)
        outcome_ = tr("Left with %1 of %n file(s) outstanding. Drafts stay in Branch Truce; the Git operation was not "
                      "continued or committed. %2",
                      nullptr, summary.totalFiles)
                       .arg(summary.outstandingFiles)
                       .arg(tr("%n launched path(s) returned to Git unresolved.", nullptr, launches));
    else
        outcome_ = tr("Left with %1 of %n file(s) outstanding. Drafts stay in Branch Truce; the Git operation was not "
                      "continued or committed.",
                      nullptr, summary.totalFiles)
                       .arg(summary.outstandingFiles);
    Q_EMIT changed();
    Q_EMIT sessionEnded(outcome_, summary.clean);
    return true;
}

bool SessionCompletion::retryPendingRecovery()
{
    std::vector<std::string> pending;
    for (const auto &file : store_.files())
        if (store_.pendingAction(file.id())) pending.push_back(file.id());
    for (const auto &id : pending) (void)store_.retryRecovery(id);
    return !store_.hasPendingRecovery();
}
}
