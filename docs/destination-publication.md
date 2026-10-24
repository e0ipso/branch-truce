# Destination publication

`src/storage/DestinationWriter.h` provides synchronous capture and publication operations for a Linux worker. It does not save drafts or stage Git entries. `ResolutionPublisher` persists the reviewed draft and intent, validates the operation/index/conversion policy, and serializes mutations through `SessionCoordinator` before calling it. A successful filesystem write alone never means Git resolution succeeded.

Capture the destination at launch or after a verified publication, before editing. The snapshot holds the initial bytes, metadata and open descriptors. Keep that snapshot across retries. Recapturing a failed stale baseline without the user's review would accept an unexpected external edit. A successful publication returns the replacement's baseline for the next edit.

## Containment and identity

The root must be an absolute canonical path that round-trips through Qt's filesystem encoding. The relative path remains `QByteArray` throughout native operations. It rejects empty components, absolute paths, NUL, `.`/`..`, and `.git` components. Display labels never become writable paths. Spaces, newlines, colons, leading dashes and non-UTF-8 filename bytes retain their literal identity.

Starting at `/`, capture opens every directory component with `openat`, `O_DIRECTORY` and `O_NOFOLLOW`. It opens only an existing regular destination, without following its final symlink. Descriptors pin all captured inodes. Reopening the chain and comparing device/inode, owner/group and permission bits detects root, ancestor and parent replacement. Directory timestamps are intentionally excluded because temporary publication entries change them. File checks compare identity, size, owner/group, permissions, link count, nanosecond modification/change times and exact bytes, with metadata checks around every read.

Symlinks, absent destinations and nonregular files are rejected. There is no path normalization, symlink following, directory creation for missing parents or creation of a missing destination.

## Permissions and complete replacement

Publication checks real write access by opening the destination without truncation. It also needs write/search access to the parent directory to create its private temporary directory. A failed access check leaves the destination unchanged. The caller must supply the agreed regular Git mode, `100644` or `100755`, matching the captured executable state.

The writer preserves the file's owner, group and basic permission bits. Files owned by another user, multiple hard links, special permission bits, extended attributes or ACLs are explicitly unsupported. It also rejects inherited extended metadata on the replacement instead of silently dropping or changing that metadata.

After mutation admission, the writer creates a random directory with mode `0700` beside the destination, writes a new file completely, preserves permissions, rereads its bytes and syncs it. Short writes are completed; write/read/sync failures discard only the private replacement. There is no in-place write and no fallback to a weaker operation.

The final preflight repeats the original baseline checks. `renameat2(RENAME_EXCHANGE)` then atomically exchanges the complete new file with the destination. The actual displaced entry stays in the private directory as `previous`. Filesystems without exchange support fail explicitly. After exchange, the writer checks the displaced entry against the captured inode, metadata and bytes, checks that the new destination is the prepared inode with the complete result, validates the original directory chain again, and syncs both directories. Exchange itself updates ctime, so post-exchange baseline comparison permits that one timestamp change.

## Races, failure and retry

Linux has no conditional rename that compares an existing inode and bytes. Another process can change a name after the last preflight. Descriptor-relative exchange prevents a substituted ancestor symlink from redirecting name resolution. Post-exchange inspection detects a changed destination entry or moved/replaced directory chain. It cannot prevent an uncooperative writer from moving an already opened directory outside the worktree, or changing a file again after the final inspection.

Before exchange, failures report `Unchanged`. After exchange, an unverified or unsynced result reports `Uncertain`, retains the displaced entry, and does not authorize staging. Ambiguous exchange errors, including an I/O error or lost filesystem reply, also report `Uncertain` and preserve the exchange slot. Its contents may be the prepared result or the displaced entry until inspected. There is no automatic rollback: a compensating rename could overwrite another external edit. `retainedPath` contains native filename bytes and is only a location hint if an ancestor moved. Reinspect the filesystem and retained entry before offering another publication. Do not delete or restore anything solely from that hint.

`Published` means that the new destination was observed and synced, not staged. A cleanup failure can accompany `Published`; it still blocks an overall success and staging. Only a failure-free result returns a new baseline. The writer accepts the serialized result by const reference, so cancellation, stale-state rejection and publication failure leave the caller's draft available. Retry an unchanged transient failure with the same baseline while all captured bytes and metadata still match. File permission or content changes can invalidate that baseline even if their prior values are restored, because ctime changes. Such mismatches and uncertain outcomes need inspection and an explicitly reconciled baseline first.

The private directory protects intermediate files from other users. This is not a sandbox against the same user or root deliberately tampering with private entries. The writer refuses observed tampering and keeps uncertain state rather than claiming an atomic transaction with Git. Crash recovery of mutation intent and retained directories belongs to the caller's recovery protocol.

## Implementation evidence

A disposable Qt 6.10.2 filesystem prototype with direct-write fallback disabled demonstrated that a path precheck followed by `QSaveFile` can report successful commit while overwriting an external edit, following a substituted destination symlink, or writing through a substituted parent symlink. Its read-only destination case failed as expected. That ruled out treating `QSaveFile::commit()` as destination validation.

Disposable native filesystem/Git fixtures check containment, byte paths, executable permissions, stale bytes and inodes, root/parent/file substitution, write refusal, cancellation and unchanged Git index entries. Linker syscall wrappers inject short/interrupted writes, partial-write failure, unsupported exchange, substitution immediately before/after exchange, lost exchange replies, changes during directory sync and cleanup failure. They check complete result bytes on success, explicit uncertainty with retained concurrent bytes on races, and removal of all fixture roots using native filename bytes. These fixtures are execution evidence rather than a permanent regression suite.

The selected native calls follow the [Linux openat rules](https://man7.org/linux/man-pages/man2/open.2.html) and [atomic exchange semantics](https://man7.org/linux/man-pages/man2/rename.2.html). [QSaveFile](https://doc.qt.io/qt-6/qsavefile.html) remains suitable for app-owned files whose path identity is not contested. Permanent race/fault regression coverage belongs to the wave verification gate; no conversion format matrix or shared canonical-output budget is introduced here.
