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
    case Transition::InvalidAnchor:
    case Transition::PreviewChanged: show(tr("The target is no longer valid. Select the scope again.")); break;
    case Transition::TextTooLarge: show(tr("The result would be over the supported size. Nothing changed.")); break;
    case Transition::InvalidText: show(tr("The text is not valid UTF-8 with LF line endings. Nothing changed.")); break;
    case Transition::ResourceLimit: show(tr("Computation exceeds the supported work limit. Nothing changed.")); break;
    case Transition::ComputationFailed: show(tr("Could not compute this revision. Nothing changed.")); break;
    case Transition::OutOfMemory: show(tr("Not enough memory to complete the command. Nothing changed.")); break;
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
    if (!markers_) return {};
    const auto &markers = *markers_;
    const auto *file = store_.file(markers.fileId);
    if (!file || selection_.id() != markers.fileId || file->current().id != markers.revision) return {};
    return {{QStringLiteral("fileId"), QString::fromStdString(markers.fileId)},
            {QStringLiteral("revision"), QString::number(markers.revision)},
            {QStringLiteral("count"), QVariant::fromValue(static_cast<qulonglong>(markers.count))},
            {QStringLiteral("line"), QVariant::fromValue(static_cast<qulonglong>(markers.line))}};
}
}
