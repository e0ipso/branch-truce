# Recovery journal version 1

The `RecoveryJournal` that each `SessionModel` owns keeps automatic recovery under
`<GenericDataLocation>/Branch Truce/recovery/v1/<session-uuid>/`, normally
`~/.local/share/Branch Truce/recovery/v1/<session-uuid>/`. The root does not
depend on the organization or application name. Each model
starts with a new store. Startup adopts a validated inactive store for the same
operation when exactly one matches, or when exactly one of several matches holds
app edits. Tests and adapters can supply an absolute store directory.
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
unresolved-marker count. Replay scans the restored text again instead of
trusting that count, so a record whose count differs from the scan does not
match. A revision with hand-edited anchors lists their
contribution IDs in `handEdited`. A revision without the field has none, which
covers records written before the field existed. It also records the separate
manual Save checkpoint, review declaration, written/staged revisions and
operation outcomes. All 64-bit integers are decimal strings. Decision values,
outcomes and source format enums use the values declared by the version 1
domain types.

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
blocked and recovery failure stays visible. A current load can replace its
initial unreadable placeholders with the captured files, including their
pending open records. Recovery retry then commits the captured sources and
result, rather than the placeholders. Pending edits still prevent session
replacement.

For every applied model command:

1. Apply the domain command to a private candidate, retaining the acknowledged
   model state. Text and decisions are one candidate, including undo and redo.
2. Commit and validate all referenced payloads.
3. Append one complete journal frame under the writer lock. Flush and
   synchronize the journal file.
4. Publish the candidate to the model, emit change notifications and return
   `Transition::Applied`. This is the acknowledgment boundary.

No editor mutation is acknowledged through this model before step 3 succeeds.
Exact manual edits, scoped choices and mapped editing commands serialize their
complete resulting draft as `replace-draft` records. Replay therefore rebuilds
the same action order before applying durable undo and redo records. A draft
replacement cannot tell typing from a choice, so replay takes each revision's
hand-edit flags from its record.
Committed IME input uses that same acknowledgment boundary. Composition/pre-edit
stays in the editor as visibly pending input until it becomes a command.
Undo grouping cannot defer persistence. The desktop editor routes document
changes through this boundary, and its native undo stack is disabled so editing
and scoped choices share application history.

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
Startup inventories inactive stores under the versioned recovery root. A
session lock prevents another window from adopting an active store. Corrupt,
unsupported or ambiguous records produce diagnostics, never a prefix restore.
Operation identity includes the canonical worktree and Git directory, source
commits, base and rebase context. All captured files must share one session
identity. Unrelated worktrees are not adopted.

Diagnostics for stores that were not adopted remain visible in the session
detail without blocking edits to the current conflict files. Only failures
belonging to files in an adopted store become those files' recovery errors.
An aborted or older operation's store is retained for inspection. To retire
one, close all Branch Truce windows and move its complete `<session-uuid>`
directory outside `<GenericDataLocation>/Branch Truce/recovery/v1/`.
Keep its journal and payloads together; moving the directory back makes it
available for inspection again.

Two windows open on the same operation each keep their own store, because a
store's session lock keeps the second window from adopting the first window's
store. A store whose journal holds only launch captures (`open` records) has no
app edit, decision, Save, review or repository outcome. When several stores
match, startup sets these aside and they never make the inventory ambiguous.
Setting a store aside writes an owner-only `set-aside` marker inside it; the
journal and payloads stay in place and are never deleted. Startup skips a
set-aside store unless the user names it.

When two or more matching stores hold app edits, restoration stays blocked
rather than guessing. The current files still open in a new store, and the
diagnostic lists each session UUID with its journal's last change time. The
session loader's `recoveryChoices` lists the same sessions with the paths each one
edited, and the window shows a Restore button for each. To clear the block,
choose a session there, or close every Branch Truce window and start Branch
Truce with `--recovery-session <session-uuid>`. Either way startup adopts that
store and sets aside every other matching store. Naming a set-aside session
the same way removes its marker and restores it, which sets the others aside in
turn. If the named session is not restored, because it does not match the
operation, another window holds it, or its marker cannot be removed, startup
reports why and restores no other store with app edits in its place.
Restoration stays blocked and lists every remaining store with app edits as a
choice, even when only one remains, and none of them is set aside. A single
remaining store that holds only launch captures is still adopted, because it
holds no work to choose between.

The window choice (`chooseRecoverySession`) moves the window to a new store and
reloads the current files through the same restore. It is refused while another
window holds one of the listed sessions, and while this window's own store
already holds app edits, pending input or a running resolution, because the
reload would leave that work in a store nobody chose. The check releases its
locks before the reload, so another window can still take the chosen session
in between; the reload then stays blocked as described above. A `git mergetool`
launcher that waits during the choice stays attached. After the reload its path
is handed to Git again, so Mark resolved releases the launcher instead of
staging the path in the app.

A store can capture a path before Git launches it. Git then moves `MERGED`
aside and copies it back, so the file gets a new inode and new timestamps with
the same bytes. The restore treats only that change as a copy: bytes, mode,
size, device, index entries and conversion policy must all still match the
launch capture, and the files loaded for the restore must already show the
copy. The path keeps a recovery error until a launcher waits on it whose
`MERGED` copy holds the captured launch bytes. A launcher can attach long after
the restore, so Git then inspects the path's index entries and conversion
policy again in the background. The error clears only when the file still has
the identity the restore validated and the index entries and policy still
match the capture. Then the copy becomes the launch destination, and Mark
resolved hands the path back to Git. This happens right after an in-window
choice, or when the launcher attaches to a window it started. Any other change
keeps the error: different bytes, mode or index entries, a changed conversion
policy, or a file, index or policy that changed after the restore, such as a
staged path or a new `core.autocrlf`. Leaving such a path returns it to Git
unresolved.

The restore adapter replays the existing domain transitions and compares every
complete state with its canonical encoding. This validates the state schema,
enums, payload references, revisions and outcome relationships. Records are
applied in journal order, including durable discard transactions. It preserves
the launch capture and the manual Save baseline independently of the current
draft. Restoration does not create a Save checkpoint or a review declaration.

Before adoption, read-only inspection checks the current destination, source
stages or recorded staged object, modes and conversion policy. Validated drafts
whose live disk/index evidence has changed remain visible with recovery errors
and cannot acknowledge edits or claim completion. Pending input from a failed
commit remains distinct from these restored recovery failures. The interface
shows unsaved, review-needed and unresolved status and retains pending text
separately from the acknowledged result.

A restored file's recovery error blocks editing, choices, history and
repository mutations for that file. Other files remain editable, including
after another restart. The session cannot finish while any recovery error
remains. Pending recovery commits still block commands across the shared
store until retry succeeds.

The coordinator commits a mutation intent before admitting publication or
staging, and records its inspection observation before releasing the queue.
These records retain the original target even after cancellation or session
replacement. Mutation observations never replace a later draft or discard.
An interrupted mutation is inspected against disk/index evidence without
retrying or rolling back. Matching result bytes alone cannot establish writer
cleanup or Git resolution; missing completion evidence remains uncertain and
outstanding. Interrupted staging can be recorded as succeeded when the exact
canonical object and mode, conversion policy, written result and recorded
review all match. That recovered observation is committed before the model
exposes resolution. Previously recorded successful outcomes must still match live
disk and index evidence. Reopened undo stacks are not a product guarantee.

The guarantee covers application termination after acknowledgment, including
unsaved edits and decisions. Termination before acknowledgment may recover a
complete candidate whose notification had not yet occurred. An interrupted
trailing record is pending, never an acknowledged command. This protocol does
not promise recovery from machine power loss. Orphan payloads left before a
record commit are harmless and are retained; garbage collection is separate.

The initial implementation scans and verifies the journal before each append.
A recovery payload larger than 8388608 bytes is refused before it is written.
The prior acknowledged record stays in the journal, and the failure says that
pending input is retained. Compaction and checkpoint replacement remain separate.
