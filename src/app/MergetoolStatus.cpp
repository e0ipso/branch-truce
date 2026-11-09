// The mergetool status shows whether Git waits on the selected path and who stages it.
#include "MergetoolStatus.h"
#include "CommandGuard.h"
#include "Selection.h"
#include "SessionModel.h"
#include "mergetool/MergetoolSession.h"

namespace branch_truce {
MergetoolStatus::MergetoolStatus(MergetoolSession &session, ResolutionPublisher &publisher, const SessionModel &files,
                                 const Selection &selection, QObject *parent)
    : QObject(parent), session_(session), publisher_(publisher), files_(files), selection_(selection)
{
    connect(&session_, &MergetoolSession::changed, this, &MergetoolStatus::changed);
    // A publication step can set or drop the parent staging offer.
    connect(&publisher_, &ResolutionPublisher::changed, this, &MergetoolStatus::changed);
    // The waiting state and the staging path follow the selection.
    connect(&selection_, &Selection::changed, this, &MergetoolStatus::changed);
}

QString MergetoolStatus::parentStagingPath() const
{
    if (!publisher_.parentStagingOffer(selection_.id())) return {};
    return files_.displayPath(selection_.id());
}

bool MergetoolStatus::waiting() const
{
    const auto *file = files_.store().file(selection_.id());
    return file && session_.waitingOn(QByteArray::fromStdString(file->path()));
}

bool MergetoolStatus::parentSessionActive() const { return session_.parentSessionActive(); }

bool MergetoolStatus::canEndParentSession() const { return session_.canEndParentSession(); }

bool MergetoolStatus::leave()
{
    const auto *file = files_.store().file(selection_.id());
    return file && session_.leave(QByteArray::fromStdString(file->path()));
}

bool MergetoolStatus::endParentSession() { return session_.endParentSession(); }

bool MergetoolStatus::recheckParentAcknowledgment()
{
    const auto offer = publisher_.parentStagingOffer(selection_.id());
    return offer && verifyParentAcknowledgment(selection_.fileId(), QString::number(offer->revision));
}

bool MergetoolStatus::assignParentStaging(const QByteArray &path) { return publisher_.assignParentStaging(path); }

bool MergetoolStatus::verifyParentAcknowledgment(const QString &fileId, const QString &revision)
{
    const auto expected = parseRevisionId(revision);
    return expected && publisher_.verifyParentAcknowledgment(fileId.toStdString(), *expected);
}

std::optional<ResolutionPublisher::Offer> MergetoolStatus::parentStagingOffer(const QString &fileId) const
{
    return publisher_.parentStagingOffer(fileId.toStdString());
}
}
