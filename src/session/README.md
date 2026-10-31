# Session capture and background work

ConflictCapture turns inspected Git files into immutable source snapshots. SessionCoordinator runs guarded save and publication work, fences completions by session and revision, and reconciles cancellation before another worktree mutation.

Add capture assembly and background operation coordination here. Keep persistence mechanisms in storage/, recovery replay in recovery/, and the reviewed publication sequence in publication/.
