# Session capture and background work

SessionStore owns the session's file histories and their recovery journal, and runs every command that changes them; typing it applies at once and records behind the editor. BackgroundAlignment aligns typed edits on a worker and installs the map while its revision is current. ConflictCapture turns inspected Git files into immutable source snapshots. SessionCoordinator runs guarded save and publication work, fences completions by session and revision, and reconciles cancellation before another worktree mutation.

Add file-history commands, capture assembly and background operation coordination here. Nothing here depends on app/ or QML; app/ presents the store. Keep persistence mechanisms in storage/, recovery replay in recovery/, and the reviewed publication sequence in publication/.
