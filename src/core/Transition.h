// Session command results and the support state that gates them.
#pragma once

namespace branch_truce {

// LargeFile keeps a captured file with more than kLargeFileLines lines unloaded
// until the user confirms it with FileSession::loadLargeFile().
enum class Support { Supported, OutsideApp, Unreadable, LargeFile };

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
