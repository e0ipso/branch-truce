# Mergetool launcher

This is the transport for Git's mergetool parent. Compatibility is declared
only for the configuration and behaviour listed under "Tested with a real
parent" below. It is not a Kubuntu 26.04 claim. For setup and day-to-day use,
see [mergetool.md](mergetool.md).

Installed Git 2.43 runs the tool once per path. Its shell sets `BASE`,
`LOCAL`, `REMOTE` and `MERGED` for the evaluated `mergetool.<tool>.cmd` and
does not export them. The command assigns those four names into the launcher
environment, then runs `branch-truce --mergetool`. With `trustExitCode`
enabled, exit 0 is followed by `git add -- "$MERGED"`. Any other exit moves
Git's backup back onto `MERGED` and deletes the temporary inputs.

## Transport

`branch-truce --mergetool` is a headless process. The open GUI session listens
on a same-user local socket. The session key is the SHA-256 of the canonical
worktree root, the operation (`merge` or `rebase`) and the two stage commit
ids. The socket is `bt-mt-` plus the first 32 hex characters of that key. A
second GUI does not steal a live socket; it says that another window receives
the launches.

The socket does not depend on `TMPDIR`. The GUI and the launcher use the same
lookup, `mergetoolSocketDirectory()`:

1. `$XDG_RUNTIME_DIR/branch-truce`, when `XDG_RUNTIME_DIR` is absolute and the
   whole socket path stays within 106 bytes. A Unix socket address holds 107,
   and Qt reserves one more byte on Linux.
2. Otherwise `/tmp/branch-truce-<uid>`.

The GUI creates that directory with mode 0700 and refuses one that is not a
real directory, is owned by another user or grants group or other access. The
Linux abstract socket namespace is not used, because it has no filesystem
permissions. Before sending its attach frame, the launcher checks with
`SO_PEERCRED` that the listening process runs as the same user.

When the session cannot listen, the startup state is `ready-without-mergetool`
instead of `ready`, and the detail starts with "Mergetool attach is
unavailable:" and the reason. The files stay open in the app. A launcher for
that worktree exits 1 with "No Branch Truce session is listening for this
repository." and Git restores `MERGED`.

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
captured snapshot. Drafts and Save checkpoints stay in Branch Truce. A hard
quit, which destroys the session, drops open waiters without success. Closing
the window while work is outstanding opens a handoff dialog instead.
Its deliberate leave, `leaveSession`, keeps drafts in the recovery store, sends
an explicit leave to every waiting launcher and runs no Git command. While
the parent session is active, Mark resolved on a path Git has not launched
waits instead of staging it. `endParentSession` returns remaining files to app-owned staging only when
no waiter is blocked and no parent offer is still unverified.

If nothing is listening, the launcher can start the GUI on the discovered
worktree and wait for the socket. It passes the launched path, hex-encoded as
in the attach frame, with the hidden `--mergetool-path` option, so the window
opens on that file. The started GUI's standard input, output and error go to
`/dev/null`, so it writes nothing into the `git mergetool` terminal.
`BRANCH_TRUCE_MERGETOOL_NO_SPAWN` disables that start.

## Tested with a real parent

`git.mergetool-parent` (`tests/git/MergetoolParentTests.cpp`, fixture
`tests/fixtures/mergetool/RealParent.h`) runs `/usr/bin/git mergetool
--no-prompt --tool=<tool>` against the `branch-truce` launcher and a session
in the test process. Only Git 2.43.0 (Ubuntu package 1:2.43.0-1ubuntu7.4)
on TUXEDO OS 24.04 was exercised, on 2026-10-25. No other Git version was run.

Each case creates a throwaway repository with a real `git merge` conflict.
`HOME`, the XDG directories and `GIT_CONFIG_GLOBAL` point into temporary
directories, `GIT_CONFIG_NOSYSTEM=1`, `PATH=/usr/bin:/bin` and `LC_ALL=C`.
The user's Git configuration and repositories are never read. This is the
exact tested global configuration, with the launcher path single-quoted:

```ini
[mergetool "truce"]
	cmd = MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" '/path/to/branch-truce' --mergetool
	trustExitCode = true
[mergetool]
	keepBackup = false
	keepTemporaries = false
[core]
	autocrlf = false
[init]
	defaultBranch = main
[user]
	name = Branch Truce Tests
	email = tests@example.invalid
```

`mergetool.writeToTemp` is unset, so Git writes its temporaries next to
`MERGED`. Two test-only tools run the same command and then do more in the
parent's shell. `truce-gated` records the launcher's exit status and holds
the parent before Git stages or restores. `truce-tamper` overwrites `MERGED`
after a successful launcher exit. Neither is a recommended setup.

These behaviours pass and are the compatibility declaration:

- Save writes a checkpoint in the app store only. While Git stays inside
  the tool command, `MERGED` keeps its launch bytes and `git ls-files
  --stage` does not change.
- After Mark resolved the launcher exits 0, and Git's own `git add` stages
  the path. With the parent held before that `git add`, the session does not
  report the path resolved. It reports it once the index holds the offer's
  canonical object id, which equals `git hash-object` of the reviewed text.
- If the parent stages other bytes, the session reports "Written,
  unresolved. The index does not contain the reviewed result."
- Leave (`leaveMergetool` on the waiting path), a hard quit that destroys
  the session, and a terminal interrupt (SIGINT to Git's process group) all
  fail the tool without staging. After leave or a hard quit,
  `MERGED` holds the launch bytes again, even after an outside edit during the
  wait. After an interrupt `MERGED` keeps the launch bytes, and Git 2.43 leaves
  its BACKUP, LOCAL, REMOTE and BASE files in the worktree because it has no
  interrupt trap. The saved draft stays in the session, and a new session on
  the same repository restores it.
- `leaveSession` sends the waiting launcher an explicit leave. The launcher
  exits 1 without reporting a missing decision, Git prints "merge of <path>
  failed" and stages nothing. `MERGED` holds the launch bytes again, even
  after an outside edit during the wait. With one launched path `git
  mergetool` exits 1 and `git ls-files --stage` does not change. With no
  paths given and three unmerged paths, the first resolved and staged before
  the leave, the second gets the leave. Git then asks whether to continue,
  and after "n" it exits 1 with the index as it was before the leave and the
  third path never launched. A new session on the same repository restores
  every saved draft, including one for a path Git never launched. The
  window-close handoff dialog is not covered by a real parent.
- Choosing a blocked recovery session in the window while Git waits keeps
  the launcher attached. The chosen store captured the launched path before
  Git's backup copy gave `MERGED` a new inode. The launcher's `MERGED` copy
  holds the captured launch bytes, so the restored draft stays editable, and
  Mark resolved makes the launcher exit 0 and Git stages the reviewed object.
- An outside edit of the launched path before that choice leaves the restored
  draft with a recovery error, and Mark resolved refuses it. Git stays blocked
  and nothing is staged. Leave then makes the launcher exit 1, `MERGED` holds
  the launch bytes and `git ls-files --stage` does not change.
- Each invocation gets its own intake. Its copies match Git's LOCAL, REMOTE
  and BASE files while the tool runs, and stay after Git deletes those files.
- Running `git mergetool` with no paths visits unmerged paths in name order.
  Mark resolved on the second path waits. That path is not offered while Git
  is held before staging the first, and it stays blocked and unacknowledged
  after the first is staged, even when both resolve to the same object.

## Trust boundaries

Git evaluates the configured `cmd` as `( eval $merge_tool_cmd )`. The value
is unquoted, so the shell splits it into words and expands filename patterns
before `eval`. A literal newline or a run of spaces inside quotes becomes one
space. Keep the launcher path free of whitespace runs and glob characters.

The parent shell and everything it runs before the launcher are outside the
app's control. Git has already written `MERGED`, its backup and its
temporaries, and has run any checkout conversion or filesystem monitor
configured at that point. The launcher trusts the four paths it receives and
copies their bytes. The app's guards protect only the staging handoff that
follows. The real-parent fixture covers one guard. When `core.fsmonitor` is set
during the wait, Mark resolved still writes `MERGED`, then refuses to release
the parent ("unsupported partial repository or filesystem monitor setting").
The monitor never runs, and leave restores the launch bytes. Guards for
filters, hooks and other helpers are covered by `git.adapter`, not by a real
parent.
