# Use Branch Truce as Git's merge tool

`git mergetool` can hand each conflicted path to Branch Truce and wait until
you either mark it resolved or leave it. This page gives the configuration
that the real-parent tests pass with, and explains who writes and who stages
the file at each step. The transport and the full list of tested behaviours
are in [mergetool-launcher.md](mergetool-launcher.md).

## Register the tool

Run these once. They write to your global Git configuration (normally
`~/.gitconfig`):

```sh
git config --global mergetool.truce.cmd \
    "MERGED=\"\$MERGED\" BASE=\"\$BASE\" LOCAL=\"\$LOCAL\" REMOTE=\"\$REMOTE\" '/usr/bin/branch-truce' --mergetool"
git config --global mergetool.truce.trustExitCode true
git config --global mergetool.keepBackup false
git config --global mergetool.keepTemporaries false
```

The result is the tool section the tests write:

```ini
[mergetool "truce"]
	cmd = MERGED=\"$MERGED\" BASE=\"$BASE\" LOCAL=\"$LOCAL\" REMOTE=\"$REMOTE\" '/usr/bin/branch-truce' --mergetool
	trustExitCode = true
[mergetool]
	keepBackup = false
	keepTemporaries = false
```

`/usr/bin/branch-truce` is where the `.deb` installs the program. For a
build tree or a custom install prefix, put that absolute path between the
single quotes instead, for example
`'/home/you/src/branch-truce/build/desktop/src/app/branch-truce'`. Git
splits the command into words and expands filename patterns before it runs
it, so a run of spaces in the path becomes one space and `*`, `?` or `[` can
expand. Use a path without either.

Some details matter:

- Git sets `MERGED`, `BASE`, `LOCAL` and `REMOTE` in its own shell but does
  not export them. The `NAME="$NAME"` assignments are how the launcher gets
  them. Do not drop them.
- `trustExitCode = true` makes Git stage the path only when the launcher
  exits 0, and restore its backup otherwise. Configurations without it have
  not been tested.
- The tests also ran with `core.autocrlf = false` and with
  `mergetool.writeToTemp` unset. Other checkout conversion settings have
  not been run under a real parent.

You can also set `merge.tool` to `truce` so that a plain `git mergetool`
picks it. The tests always pass `--tool=truce` instead.

## Run it

Start from a merge or rebase that stopped with conflicts:

```sh
git mergetool --tool=truce            # every unmerged path, in name order
git mergetool --tool=truce -- path    # one path
```

Git runs the launcher once per path and waits for it. If no Branch Truce
window has this repository open, the launcher starts one, which opens on the
path Git launched. If a window already has it open, for example one started
with `branch-truce /path/to/repository`, the launcher attaches to that
window. The window shows "Git is waiting on this path." on the waiting file.

A window the launcher starts writes nothing into the `git mergetool`
terminal. Set `BRANCH_TRUCE_MERGETOOL_NO_SPAWN=1` to stop the launcher
starting a window. It then
exits 1 with "No Branch Truce session is listening for this repository."
when none is open, and Git restores the file.

## Save and Mark resolved

Save keeps a draft inside Branch Truce. It does not write the repository
file and does not touch the index. While Git waits, the file still holds the
bytes it had when Git launched the tool, and `git ls-files --stage` does not
change. A saved draft does not count as reviewed or resolved.

Mark resolved declares that you reviewed the result on screen. If some
source choices are still undecided, the app first asks you to confirm the
result with Confirm resolution, or Cancel, which leaves text and choices as
they were. Then it saves that exact revision, writes it to the file Git
launched and tells the launcher. The launcher exits 0, and Git runs its own
`git add` on the path. If something else changed the file after Git launched
the tool, Mark resolved refuses to overwrite it.

The app never stages a path that Git launched. It shows "Written,
unresolved. Waiting for Git to stage <path>." and counts the file resolved
only once the index holds the reviewed bytes. Check Git staging repeats that
test. If Git stages different bytes, for example because something rewrote
the file after the launcher exited, the file shows "Written, unresolved. The
index does not contain the reviewed result." and stays outstanding.

Mark resolved on a path Git has not launched yet waits for Git to reach it
("Waiting for Git to launch this path. It is not resolved."). When no
launcher is waiting and the index holds every result handed to Git, Parent
Git is finished hands the remaining files back to the app. From then on the
app stages a file itself when you mark it resolved.

Neither action continues the merge or rebase or creates a commit. Run
`git merge --continue`, `git rebase --continue` or `git commit` yourself
afterwards.

## Leave, cancel and close

Every way out except Mark resolved makes the launcher exit non-zero. Git then
stages nothing for that path, and your drafts stay in Branch Truce:

- Leave unresolved, on the waiting file, sends Git an explicit leave. Git
  prints "merge of <path> failed". The file gets its launch bytes back, even
  if something else edited it during the wait. When more paths remain, Git
  asks whether to continue with them.
- Closing the window while work is outstanding opens a summary. Leave with
  work outstanding keeps every draft, sends a leave to every waiting launcher
  and runs no Git command. It stays disabled while a resolution is running
  or unsaved input has not reached the recovery store yet. Finish appears
  instead when nothing is outstanding. No real-parent test covers this
  dialog.
- If the window is killed, the launcher prints "The launcher
  closed before an explicit decision." and the file gets its launch bytes
  back.
- Ctrl+C in the terminal stops Git without staging. The file keeps its launch
  bytes, but Git 2.43 has no interrupt handler, so its `BACKUP`, `LOCAL`,
  `REMOTE` and `BASE` copies stay in the worktree for you to delete.

Opening the repository again in Branch Truce restores the saved drafts,
including drafts for paths Git never launched.

## What Branch Truce does not control

Git runs the configured command in its own shell, and that shell is outside
the app. By the time the launcher starts, Git has already written the file
and its backup and temporary copies, and has run any checkout filter, hook
or filesystem monitor configured for that step. The launcher trusts the four
paths it receives and copies their bytes. After the launcher exits, Git's
own `git add` stages the file. The app checks the result in the index but
cannot stop Git or another program changing the file before that.

The app's guards cover only the handoff it performs. When `core.fsmonitor`
is set during the wait, Mark resolved still writes the file but refuses to
release Git, and leaving restores the launch bytes. Guards for filters, hooks
and other helpers are tested against the app's own Git calls, not under a
real `git mergetool`. See
[mergetool-launcher.md](mergetool-launcher.md#trust-boundaries).

## Limits

- Physical desktop integration requires separate validation of Plasma menus,
  KWin scaling, input methods and screen readers. See the
  [platform validation procedure](platform-validation.md#physical-desktop-checks).
- The launcher and the window talk over a socket in
  `$XDG_RUNTIME_DIR/branch-truce`, or `/tmp/branch-truce-<uid>` when that path
  is too long. Both must run as the same user.
