// Session command results, the support state that gates them and why the engine held a file back.
#pragma once

#include "ResourceLimits.h"
#include <cstddef>

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
    OutOfMemory
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
    InvalidOutcome,
    InvalidAnchor,
    AmbiguousTarget
};

}
