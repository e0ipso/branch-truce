// Source commands are checked against the selected file and result revision before they can change work.
#pragma once
#include "core/SessionState.h"
#include <QString>
#include <optional>
#include <variant>

namespace branch_truce {
class SessionStore;

// A revision ID as the views pass it: the decimal form of a nonzero ID, or nothing.
[[nodiscard]] std::optional<RevisionId> parseRevisionId(const QString &value);

// The file and revision a command may run on.
struct CommandTarget {
    const FileSession *file = nullptr;
    RevisionId revision = 0;
};

// What a command needs beyond its file being selected.
enum class CommandCheck {
    // The previews compare the revision with the file themselves.
    Selected,
    // Undo and redo run on the current revision.
    Current,
    // A native edit runs on the current revision. It is the outcome of any
    // input method composition, so composing does not refuse it.
    Edit,
    // Save, Mark resolved and the discards run on the current revision of a supported file.
    CurrentSupported,
};

// The checks every result command runs first. The command runs on the returned
// target, or reports the returned refusal: StaleRevision when the file is
// unknown, the revision does not parse, another file is selected or, except
// for Selected, the file moved past that revision; Blocked while recovery
// input is pending, the file has a recovery error, an input method is
// composing (except for Edit) or, for CurrentSupported, the file is not
// supported.
[[nodiscard]] std::variant<CommandTarget, Transition> checkCommand(const SessionStore &files,
                                                                   const QString &selectedFileId, bool composing,
                                                                   const QString &fileId, const QString &revision,
                                                                   CommandCheck check);
}
