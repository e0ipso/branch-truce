// File states are the typed vocabulary the views branch on, so no view compares display text.
#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace branch_truce::FileStates {
Q_NAMESPACE
QML_ELEMENT
Q_CLASSINFO("RegisterEnumClassesUnscoped", "false")

// What a file's status says. The values up to ReviewNeeded mirror the
// engine's FileStatus; the recovery states outrank every file status.
enum class StatusKind {
    OutsideApp,
    Unreadable,
    LargeFile,
    OutcomeUncertain,
    CheckpointFailed,
    WriteFailed,
    StageFailed,
    EarlierStaged,
    Resolved,
    WrittenUnresolved,
    SavedDraft,
    Undecided,
    ReviewNeeded,
    RecoveryBlocked,
    RecoveryPending
};
Q_ENUM_NS(StatusKind)

// Whether the app can edit a file. Mirrors the engine's Support.
enum class SupportKind { Supported, OutsideApp, Unreadable, LargeFile };
Q_ENUM_NS(SupportKind)

// Where a file's edits stand in the recovery store. Recording is typing that
// is applied but not yet durable, Failed is pending input whose commit failed,
// SaveFailed the same for a Save, and Blocked a recovery error that keeps the
// file from changing.
enum class RecoveryState { Durable, Restored, Recording, Failed, SaveFailed, Blocked };
Q_ENUM_NS(RecoveryState)

// The handoff group a file belongs to, in the order the handoff lists them.
enum class HandoffCategory {
    Undecided,
    SavedDraft,
    ReviewNeeded,
    NewerDraft,
    WrittenUnresolved,
    Failed,
    OutsideApp,
    Published
};
Q_ENUM_NS(HandoffCategory)

// What the handoff offers to do next for a file.
enum class NextAction {
    None,
    OpenRecoveryProblem,
    RetrySave,
    RetryRecovery,
    ExplainOutsideApp,
    Inspect,
    LoadLargeFile,
    MarkResolved,
    ApplyOrDiscard,
    CheckGit,
    Decide
};
Q_ENUM_NS(NextAction)
}
