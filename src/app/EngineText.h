// Engine text turns the merge engine's typed statuses, failures and preview notes into translated sentences.
#pragma once

#include "core/EditingCommands.h"
#include "core/ResourceLimits.h"
#include "core/SessionState.h"
#include <QString>

namespace branch_truce::engine_text {

[[nodiscard]] QString status(FileStatus status);
[[nodiscard]] QString blocker(Blocker blocker);
// Empty for a text within the envelope.
[[nodiscard]] QString textLimit(const TextAssessment &assessment);
[[nodiscard]] QString loadFailure(const LoadFailure &failure);
// Why the file has its support: the session's own load failure, otherwise
// the adapter's detail, which may be empty.
[[nodiscard]] QString supportDetail(const FileSession &file);
[[nodiscard]] QString choice(const ChoicePreview &preview);
[[nodiscard]] QString batch(BatchNote note);
[[nodiscard]] QString wholeFile(WholeFileNote note);

}
