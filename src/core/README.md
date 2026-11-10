# Merge engine

The Qt-free merge engine owns immutable source snapshots, result revisions,
passage correspondence, conflict markers, source choices, decision state, undo
history and publication outcomes. It performs no I/O, reads no clock and keeps
no global state. Correspondence.md and EditingCommands.md describe its range
and command contracts.

## Units

Each unit depends only on the ones listed before it.

- `ResourceLimits.h`, `ByteRanges.h`, `RevisionId.h`, `Draft.h`,
  `FileSnapshot.h`, `Transition.h`: limits, byte ranges, drafts and decisions,
  captured sources, command results and typed load failures.
- `Utf8`: the UTF-8 and character-boundary checks every unit applies.
- `Correspondence`: the `CorrespondenceAlgorithm` interface, and the map that
  aligns source and result text through one and maps source anchors into one
  result revision. `AnchorRebuild.h` holds how a choice that drops or
  rebuilds text moves those anchors.
- `ExactLineCorrespondence`: the default algorithm, an exact line LCS within
  the `kMaxAlignmentCells` budget.
- `ConflictMarkers`: recognizes marker lines of a given marker size and
  assembles blocks.
- `ChoiceCatalogue`: the source scopes a user can accept or ignore, indexed by
  contribution and section, and the non-conflicting changes.
- `MarkerResolution`: ties marker blocks to choice sections and plans the
  edits that resolve them.
- `RevisionHistory`: shared immutable revisions and the undo cursor, keeping
  only reachable revisions within `kMaxHistoryBytes`.
- `PublicationState`: Save, Mark resolved, write and stage outcomes and the
  facts, blockers and status derived from them.
- `SessionState` (`FileSession`): one file's session, composing the units
  above behind the command API.
- `EditingCommands`: previews and applies choices, non-conflicting batches and
  whole-file replacements as `EditingCommand`s.
- `WordDifferences` (token diff) and `FileWordDifferences` (word ranges of one
  revision).

`BuildInfo` is a separate library, `branch_truce_build_info`, so a version
change rebuilds nothing in the engine.

## Rules

- Engine sources include only engine headers and the standard library.
  `core.include-boundary` fails the test run otherwise.
- The engine returns enums and numbers, never user-facing sentences. The app
  turns them into translated text in src/app/EngineText.cpp. Support details
  hold only an adapter's own explanation.
- Input from adapters is validated at the boundary: text must be UTF-8 with LF
  line endings inside the envelope of ResourceLimits.h, decision orders belong
  to accepted decisions, and commands name registered anchors. Invalid input
  returns a `Transition` or opens the file as unsupported with a
  `LoadFailure`; exceptions are left for programming errors and
  `std::bad_alloc`.
- Values are immutable once shared. A `FileSession` is used by one thread at a
  time and may be moved to another thread once, as the load worker does.
  `CorrespondenceMap` and the revisions it belongs to are immutable after they
  are published, and `CorrespondenceAlgorithm::compare` may run on several
  threads at once. An edit made with `Realignment::Deferred` leaves its map's
  passages provisional: a worker may build `realigned()` from the shared map
  while the owner keeps editing, and the owner installs it with
  `FileSession::installAlignment`, which keeps it only for the revision it was
  made for. The engine runs no event loop and starts no thread.
- Work is bounded: no text over the envelope, no alignment over
  `kMaxAlignmentCells`, and no undo history over `kMaxHistoryBytes`, which
  counts every revision a publication outcome names.

Translate Qt positions, file paths and process results at an adapter boundary
instead of adding platform dependencies to the engine.
