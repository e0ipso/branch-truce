# Revisioned session state

`core/SessionState.h` is C++20 value state with no Qt, filesystem or Git
dependency. `app/SessionModel.h` adapts it to a Qt list model exposed as
`applicationController.files`. Loading files and performing actual storage,
editing commands and Git operations belong to later adapters. This change
does not make the startup window a working conflict resolver.

## Revisions and history

A file has a stable session ID and repository-relative byte path. Each draft
transaction contains the complete result text, decisions keyed by stable
contribution ID, explicit combination order and the count of known remaining
conflict-marker ranges. Callers supply marker knowledge; this model does not
scan text or infer source choices from it.

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

`Outcome::Uncertain` represents timeouts or other uninspected mutation results.
It blocks finish until an adapter inspects the outcome. Successful snapshots
and the most recent operation outcomes are separate: a failed newer write or
stage must not erase the earlier successful snapshot or claim the current
result is resolved. Adapters must serialize operations for a file, including
retries of one revision; callbacks do not have per-operation request tokens.
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
produces "Earlier result staged; current draft not applied".

The Qt model exposes snapshots, outcomes and independent facts through named
roles. Revisions and combination order use decimal strings to preserve 64-bit
identity in JavaScript. `pathBytes` retains the original bytes; `path` is the
UTF-8 display form. Each applied transition emits `dataChanged` for its row and
`summaryChanged` for aggregates, preserving persistent row indexes.

`canFinish` requires at least one loaded file and current verified resolution
of every file. `finishBlockers` includes each file ID and display path for
navigation. Empty sessions are blocked. Explicit leave after a warning belongs
to the future interface and must not claim successful resolution.
