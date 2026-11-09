// Command feedback is the one message the window shows about the last command, and the markers that refused it.
#include "CommandFeedback.h"
#include "Selection.h"
#include "session/SessionStore.h"

namespace branch_truce {
CommandFeedback::CommandFeedback(const SessionStore &store, const Selection &selection, QObject *parent)
    : QObject(parent), store_(store), selection_(selection)
{
    connect(&selection_, &Selection::changed, this, &CommandFeedback::changed);
}

void CommandFeedback::show(const QString &message)
{
    message_ = message;
    markers_.reset();
    Q_EMIT changed();
}

bool CommandFeedback::report(Transition result)
{
    switch (result) {
    case Transition::Applied: show({}); break;
    case Transition::NoChange: show(tr("No change.")); break;
    case Transition::StaleRevision: show(tr("The result changed. Select the scope again.")); break;
    case Transition::ConfirmationRequired: show(tr("Inspect and confirm the exact replacement first.")); break;
    case Transition::AmbiguousTarget: show(tr("The target is ambiguous. Edit the result directly.")); break;
    case Transition::InvalidAnchor: show(tr("The target is no longer valid. Select the scope again.")); break;
    default: show(tr("Command not completed. Check pending recovery and the selected scope.")); break;
    }
    return result == Transition::Applied || result == Transition::NoChange;
}

void CommandFeedback::showMarkers(const QString &message, const std::string &fileId, RevisionId revision,
                                  std::size_t count, std::size_t line)
{
    message_ = message;
    markers_ = Markers{fileId, revision, count, line};
    Q_EMIT changed();
}

QVariantMap CommandFeedback::markerBlocker() const
{
    const auto *file = markers_ ? store_.file(markers_->fileId) : nullptr;
    if (!file || selection_.id() != markers_->fileId || file->current().id != markers_->revision) return {};
    return {{QStringLiteral("fileId"), QString::fromStdString(markers_->fileId)},
            {QStringLiteral("revision"), QString::number(markers_->revision)},
            {QStringLiteral("count"), QVariant::fromValue(static_cast<qulonglong>(markers_->count))},
            {QStringLiteral("line"), QVariant::fromValue(static_cast<qulonglong>(markers_->line))}};
}
}
