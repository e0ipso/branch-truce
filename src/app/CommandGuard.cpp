// Source commands are checked against the selected file and result revision before they can change work.
#include "CommandGuard.h"
#include "SessionModel.h"

namespace branch_truce {
std::optional<RevisionId> parseRevisionId(const QString &value)
{
    bool ok = false;
    const auto id = value.toULongLong(&ok);
    return ok && id != 0 && QString::number(id) == value ? std::optional<RevisionId>{id} : std::nullopt;
}

std::variant<CommandTarget, Transition> checkCommand(const SessionModel &files, const QString &selectedFileId,
    bool composing, const QString &fileId, const QString &revision, CommandCheck check)
{
    const auto *file = files.file(fileId.toStdString());
    const auto id = parseRevisionId(revision);
    const bool current = check == CommandCheck::Selected || (file && id && file->current().id == *id);
    if (!file || !id || fileId != selectedFileId || !current) return Transition::StaleRevision;
    const bool supported = check == CommandCheck::Selected || file->support() == Support::Supported;
    if (!supported || files.hasPendingRecovery() || files.hasRecoveryFailure(file->id()) || composing)
        return Transition::Blocked;
    return CommandTarget{file, *id};
}
}
