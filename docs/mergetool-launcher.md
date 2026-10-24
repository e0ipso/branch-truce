# Mergetool launcher

This is the prototype transport for Git's mergetool parent. It is not a
compatibility claim, a Kubuntu 26.04 claim, or a configuration recipe. The
real-parent verification gate is still outstanding.

Installed Git 2.43 runs the tool once per path. Its shell sets `BASE`,
`LOCAL`, `REMOTE` and `MERGED` for the evaluated `mergetool.<tool>.cmd` and
does not export them. The command assigns those four names into the launcher
environment, then runs `branch-truce --mergetool`. With `trustExitCode`
enabled, exit 0 is followed by `git add -- "$MERGED"`. Any other exit moves
Git's backup back onto `MERGED` and deletes the temporary inputs.

## Transport

`branch-truce --mergetool` is a headless process. The open GUI session listens
on a same-user local socket. The socket name is `bt-mt-` plus the SHA-256 of
the canonical worktree root, the operation (`merge` or `rebase`) and the two
stage commit ids. A second GUI does not steal a live socket.

The launcher discovers that same identity, copies the temporary inputs and the
initial `MERGED` bytes into `$XDG_DATA_HOME/branch-truce/mergetool/<session>/<invocation>/`,
then sends one attach frame. The session checks those bytes against the
manifest before accepting the waiter. Copies stay after the launcher exits.
Each path has at most one live waiter. A repeated attach for that path is
rejected and does not release the first waiter. Another path can wait, but it
is not assigned until the previous path's offer has been acknowledged.

Frames are a 4-byte big-endian length plus one compact JSON object, at most
65536 bytes. The session replies `attached`, `rejected`, `resolved` or `leave`.
`resolved` carries the published revision and canonical object id.

## Lifecycle

The launcher blocks until that reply. It returns 0 only for `resolved`. Save
never writes `MERGED` or the index. Before the tool runs, installed Git moves
`MERGED` aside and copies those bytes back. Mark resolved writes that copy when
the bytes are still the launch snapshot, refuses a different file, and leaves
the path unresolved; the `resolved` reply lets the parent stage it. The GUI
then polls `verifyParentAcknowledgment` and claims resolution only when the
index holds that exact object. A still-unmerged index stays written and
unresolved.

Leave, a rejected second attach, and a dropped launcher return non-zero. Only
the waiter that was accepted restores `MERGED` when its bytes differ from the
captured snapshot. Drafts and Save checkpoints stay in Branch Truce. Closing
the GUI drops open waiters without success. While the parent session is
active, Mark resolved on a path Git has not launched waits instead of staging
it. `endParentSession` returns remaining files to app-owned staging only when
no waiter is blocked and no parent offer is still unverified.

If nothing is listening, the launcher can start the GUI on the discovered
worktree and wait for the socket. `BRANCH_TRUCE_MERGETOOL_NO_SPAWN` disables
that start. Local installed-Git checks cover success, cancellation, a repeated
invocation and multi-file blocking. They do not close the real-parent gate.
