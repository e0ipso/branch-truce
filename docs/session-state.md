# Revisioned session state

`core/SessionState.h` is C++20 value state with no Qt, filesystem or Git
dependency. `app/SessionModel.h` adapts it to a Qt list model exposed as
`applicationController.files`. `applicationController.coordinator` owns session
replacement and background execution. Repository loading, storage and editor
integration still need their adapters. This change
does not make the startup window a working conflict resolver.

## Revisions and history

A file has a stable session ID and repository-relative byte path. Each draft
transaction contains the complete result text, decisions keyed by stable
contribution ID, explicit combination order and the count of known remaining
conflict-marker ranges, which the model counts by scanning the text. It never
infers source choices from text. It removes marker lines only inside a choice
that decides a whole marker section, as `src/core/EditingCommands.md` describes.

`current().id` identifies a transaction. `result` and `decisions` identify its
text and decision content separately. Every command includes its expected
current transaction. A mismatch returns `StaleRevision` without mutation.
A no-op returns `NoChange`. A transaction changes text and decisions together.

Undo and redo restore both dimensions through the command history, issuing a
fresh transaction identity. They never restore review or change external
state. Restoring saved content retains its content identities, so it can be
manually saved while still requiring Mark resolved again. Save keeps history.
Editing after undo cuts only the redo branch; old snapshots remain available
for completed or pending operation outcomes.

All snapshots are retained for the lifetime of this in-memory file session.
History persistence and a bounded storage policy are separate work. Do not use
a returned model file pointer across row insertion, which can move the values.

## Recording outcomes

The model performs no operations. Its C++ callbacks record outcomes established
by storage and Git adapters, always for the captured transaction ID:

1. Ordinary Save calls `recordCheckpoint` after persisting that exact app
   draft. It does not declare review, write a repository file or stage anything.
2. Mark resolved calls `beginMarkResolved`. Undecided source choices require
   explicit confirmation. Known remaining markers and unavailable/unsupported
   content block it. Cancelling confirmation calls nothing.
3. After persisting the review declaration and draft, record a successful
   checkpoint for that transaction. A failed or uncertain checkpoint blocks
   publication. Review remains visible even when the checkpoint fails.
4. Record the verified repository-write outcome. Success requires that same
   reviewed and successfully checkpointed transaction.
5. Record the index outcome only after the same transaction is verified
   written. Staging success requires the adapter's opaque canonical object ID
   and mode. A child process exit alone is insufficient verification.

`ApplicationController::markResolved` runs that sequence for the selected
revision. It does not require a prior Save. Repository mode stages only that
literal path. A path assigned to parent staging is written and then left
unresolved until `verifyParentAcknowledgment` sees the canonical index entry.
The app does not stage a parent-owned path. While a mergetool parent session
is active, an unlaunched path waits instead of being staged here. The blocking
launcher and its local socket are recorded in
[the mergetool launcher note](mergetool-launcher.md).

`Outcome::Uncertain` represents timeouts or other uninspected mutation results.
It blocks finish until an adapter inspects the outcome. Successful snapshots
and the most recent operation outcomes are separate: a failed newer write or
stage must not erase the earlier successful snapshot or claim the current
result is resolved. Adapters submit mutations through `SessionCoordinator`,
including retries of one revision. Model callbacks alone do not have
per-operation request tokens.
Callbacks for superseded review declarations or deliberately discarded drafts
are rejected. Revision IDs are local to one file session, not repository IDs.

A later edit invalidates current review and retains earlier reviewed, written
and staged snapshots. Delayed outcomes for that earlier review can still be
recorded while it remains the latest review declaration, but cannot resolve
the newer draft. Git conflict stages are never recreated automatically.

## Discard and handoff

Discard to the manual checkpoint, or initial content if none exists, restores
text and decisions as a new transaction without review. Deliberate discard to
the staged result is a separate operation. It selects the previously saved and
reviewed staged identity, discarding any newer app checkpoint. It is permitted
only while both the last verified write and staging outcomes still match that
staged result. A newer repository write or uncertain outcome requires adapter
inspection or publication; selecting old text cannot pretend the disk changed.
Discard prevents pending callbacks from restoring discarded checkpoints, even
when undo has already restored the checkpoint's text and decisions. Discarding
after a failed app Save clears that failure without claiming a new saved draft.

`FileFacts` derives independent unsaved, undecided, review, earlier-staged and
resolved facts plus all finish blockers. Confirmed undecided contributions stay
visible as decisions; they stop blocking finish for that reviewed revision.
Failures, unsupported/unreadable content and known remaining markers stay
outstanding. An earlier staged result beside a different current transaction
produces "Earlier result staged; current draft not applied". An outside-app or
unreadable file has no app draft, so its blockers are its support blocker plus
any recorded Save, write, staging or uncertain-outcome failure; it never gets
draft, Mark resolved, written or staged blockers and still never resolves.

The Qt model exposes snapshots, outcomes and independent facts through named
roles. Revisions and combination order use decimal strings to preserve 64-bit
identity in JavaScript. `pathBytes` retains the original bytes; `path` is the
UTF-8 display form. Each applied transition emits `dataChanged` for its row and
`summaryChanged` for aggregates, preserving persistent row indexes.

## Session execution

`SessionCoordinator::replaceSession` takes a discovered worktree root, verified
operation identity and loaded file values. It resets the model and advances a
session generation, even when reopening the same operation. Invalid roots,
empty operation identities and duplicate file IDs leave the current session
intact. Worktree roots are canonicalized so symlink aliases share a queue.

`submit` runs expensive computation or serialization on Qt's thread pool.
Workers receive copied paths, revisions and task identities; they must not
capture model pointers or GUI objects. Completion callbacks run on the model's
owning thread. Admission and completion both check session generation,
worktree, operation and the file's transaction, result and decision revisions.
Cancellation, replaced sessions and stale revisions skip the callback entirely.
`taskFinished` reports all outcomes for diagnostics, with their disposition and
original identity. It does not authorize changes to the current model.

`submitMutation` queues app-owned mutations in request order per worktree.
Its worker calls `TaskControl::beginMutation` immediately before the first side
effect and stops when that returns false. Cancellation before admission retains
`NotAttempted`. After admission, a separate inspection runs even if cancelled
or the worker throws. The queue stays occupied through inspection. Observed
changes, observed unchanged state and uncertainty are distinct, and none imply
a rollback. An inspection exception or missing observation retains uncertainty.
`mutationOutcomes` keeps the original task's evidence across session replacement.
An uncertain outcome remains outstanding and needs further adapter inspection.

`submitGit` keeps Git on asynchronous QProcess signals. Stage requests use the
same mutation queue through the adapter's final index inspection. The controller
selects the Git executable; the coordinator supplies the directory and revision
tags, and requires staging to address the captured file path. Adapter index
observations do not prove the state of the worktree or attribute changes to one
writer. Git's locks and mutation-time baseline checks remain required.

Different worktrees can proceed independently. This queue coordinates one
application controller, not other processes. Workers cooperate with cancellation;
the coordinator never waits on them on the GUI thread. Destruction cancels
controls and disconnects delivery. In-flight mutations may still finish, so a
normal close must settle them and retain outcomes before destroying the controller.
Mark resolved publishes and verifies one reviewed revision.

`canFinish` requires at least one loaded file and current verified resolution
of every file. `finishBlockers` includes each file ID and display path for
navigation. Empty sessions are blocked.

`SessionModel::handoff` places each file in exactly one category: undecided,
saved-draft, review-needed, newer-draft, written-unresolved, published, failed
(including uncertain outcomes, unreadable files and recovery problems) or
outside-app. Each entry keeps its blockers and the next action. The controller
adds session blockers for a running resolution and for launched paths Git is
still waiting on; `clean` is the only success claim. `finishSession` succeeds
only when clean. Closing the window with work outstanding opens the handoff.
`leaveSession` is the deliberate leave: it keeps drafts in the recovery store,
sends an explicit leave to every waiting launcher and runs no Git command. It
refuses to leave while pending input is not in recovery or a resolution is
running.
