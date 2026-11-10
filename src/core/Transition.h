// Session command results, the support state that gates them and why the engine held a file back.
#pragma once

#include "ResourceLimits.h"
#include <cstddef>
#include <new>
#include <type_traits>

namespace branch_truce {

// LargeFile keeps a captured file with more than kLargeFileLines lines unloaded
// until the user confirms it with FileSession::loadLargeFile().
enum class Support { Supported, OutsideApp, Unreadable, LargeFile };

// Why FileSession itself did not open a file for editing. The app turns it
// into text; adapters explain their own decisions in the support detail.
enum class LoadFailureReason {
    // The result or a captured source is over a limit of the text envelope.
    TextTooLarge,
    // The result or a captured source is not UTF-8 with LF line endings.
    InvalidText,
    // More lines than kLargeFileLines; loadLargeFile() opens it anyway.
    LargeFile,
    // A conflict-marker-size over kMaxConflictMarkerSize.
    MarkerSizeTooLarge,
    // Aligning the sources with the result ran out of memory.
    OutOfMemory,
    // The alignment failed for any other reason. The texts were checked
    // before, so this is a defect; the file stays readable as plain text.
    AlignmentFailed
};
struct LoadFailure {
    LoadFailureReason reason;
    // TextTooLarge: the text over the envelope and the limit it passed.
    TextAssessment text = {};
    // LargeFile: lines of the longest captured text.
    std::size_t lines = 0;
    bool operator==(const LoadFailure &) const = default;
};

enum class Transition {
    Applied,
    NoChange,
    StaleRevision,
    ConfirmationRequired,
    Blocked,
    // Invalid publication/replay data, or no unused revision identity remains.
    InvalidOutcome,
    InvalidAnchor,
    AmbiguousTarget,
    // The command ran out of memory and changed nothing.
    OutOfMemory,
    // The result would be over a limit of the text envelope (ResourceLimits.h).
    TextTooLarge,
    // The text is not UTF-8 with LF line endings.
    InvalidText,
    // A preview to apply no longer matches the one the current revision gives.
    PreviewChanged
};

// Allocating commands construct their candidate before publishing it. Only
// allocation failures are expected; unrelated exceptions remain defects.
template <typename Command> auto withinMemory(Command command) -> decltype(command())
{
    try {
        return command();
    } catch (const std::bad_alloc &) {
        if constexpr (std::is_same_v<decltype(command()), Transition>)
            return Transition::OutOfMemory;
        else {
            decltype(command()) result;
            result.state = Transition::OutOfMemory;
            return result;
        }
    }
}

}
