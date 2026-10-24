# Recovery journal version 1

`SessionModel` keeps automatic recovery under
`QStandardPaths::AppLocalDataLocation/recovery/v1/<session-uuid>/`. Each model
owns a new store. Tests and adapters can supply an absolute store directory.
The store is independent of the repository, Git index and mergetool files.

## Layout and integrity

The session directory and `payloads/` have owner-only directory permissions.
Journal and payload files have owner read/write permissions. Symlink entries,
nonregular files, foreign owners and publicly accessible payloads are refused.
`journal.lock` serializes writers without waiting on the interface thread.

`payloads/<sha256-hex>` contains exact immutable bytes. This includes result
text, raw source blobs, converted source bytes and normalized text, launch
bytes, and byte-valued identities and paths. Empty payloads have a real digest;
absent sources are JSON null. Payloads are written through `QSaveFile` without
direct-write fallback, flushed and synchronized before atomic publication.
Existing payloads must still match their digest.

`journal` starts with `Branch Truce recovery journal v1` and a newline. Every
following newline-terminated JSON frame has `record` containing canonical
base64 and `sha256` containing the decoded record's SHA-256 hex digest. The
decoded JSON record has version 1, a decimal-string sequence starting at 1,
the preceding record digest, a payload digest array, and `state`.

State schema 1 contains a stable transaction UUID, action name, file identity
and exact path references, support status, immutable snapshots and the complete
current revision. A revision contains transaction, text and decision revision
IDs, its text payload, every decision and optional combination order, and its
unresolved-marker count. It also records the separate manual Save checkpoint,
review declaration, written/staged revisions and operation outcomes. All
64-bit integers are decimal strings. Decision values, outcomes and source
format enums use the values declared by the version 1 domain types.

Snapshots retain all `FileSnapshots` fields, including provenance, conversion
attributes/configuration, per-source raw and converted bytes, source labels
and commits, launch encoding/BOM/newlines, destination mode and observed file
identity. Each record lists all of its referenced payloads. Historical records
retain the launch state and earlier checkpoints even as later records evolve.

The reader validates the version, sequence, hash chain, complete frames and
every payload before returning success. A final line without a terminating
newline is interrupted pending work and is ignored. A complete corrupt record,
missing payload or unknown version returns an explicit error. Prefix records
returned alongside an error are diagnostics, not a successful restore.

## Commit and acknowledgment

Opening a file persists its capture before the model accepts edits. Loading
can still display captured content when this fails, but editing remains
blocked and recovery failure stays visible. For every applied model command:

1. Apply the domain command to a private candidate, retaining the acknowledged
   model state. Text and decisions are one candidate, including undo and redo.
2. Commit and validate all referenced payloads.
3. Append one complete journal frame under the writer lock. Flush and
   synchronize the journal file.
4. Publish the candidate to the model, emit change notifications and return
   `Transition::Applied`. This is the acknowledgment boundary.

No editor mutation is acknowledged through this model before step 3 succeeds.
Committed IME input uses the same whole-draft command. Composition/pre-edit
stays in the editor as visibly pending input until it becomes a command.
Undo grouping cannot defer persistence. The current desktop result view is
read-only; a future editable adapter must use this boundary rather than writing
directly into the domain state.

On failure the acknowledged revision stays unchanged. `pendingInput`,
`pendingText`, `pendingDecisions`, `recoveryState` and `recoveryError` expose the
retained candidate and failure separately. `resultText` and revision roles
continue to describe the acknowledged state. Further commands across the model
are blocked until `retryRecovery` or QML's `retryPendingRecovery` commits every
pending candidate.
A stable request UUID prevents duplicate appends when a failed flush left a
complete record. Incomplete trailing bytes are truncated under the lock before
retry. Session replacement and normal window close cannot drop pending input;
the close guard offers Retry or keeps the window open.

Automatic recovery never moves the manual Save checkpoint or implies review,
publication or Git resolution. A manual Save outcome is its own journal
transaction. Later automatic records retain both dirty work and that earlier
checkpoint. Discard commands commit their selected current state before
acknowledgment so earlier retained records cannot become the latest draft.

## Recovery and validation boundaries

`RecoveryStore::read` returns integrity-checked transactions, and `payload`
returns verified bytes. It does not automatically apply them. A restore caller
must validate the complete state schema and the live worktree, operation, disk
and index identities before using recovered work or allowing publication.
Recovery inventory and restoration UI are separate work. Reopened undo stacks
are not required; complete current text and decisions and the last manual Save
baseline are retained independently.

The guarantee covers application termination after acknowledgment, including
unsaved edits and decisions. Termination before acknowledgment may recover a
complete candidate whose notification had not yet occurred. An interrupted
trailing record is pending, never an acknowledged command. This protocol does
not promise recovery from machine power loss. Orphan payloads left before a
record commit are harmless and are retained; garbage collection is separate.

The initial implementation scans and verifies the journal before each append.
Local acceptance measures acknowledgment cost with disposable fixtures, but
does not set final latency, file-size or retention limits. Those operating
limits and compaction/checkpoint replacement remain validation gates.
