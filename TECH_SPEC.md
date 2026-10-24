# Branch Truce technical specification

Status: initial draft. The stack below is locked. Component contracts and implementation details are the initial design; explicitly open decisions require validation before implementation depends on them.

This document specifies how Branch Truce is built. It stands independently of [PRD.md](PRD.md), which owns product requirements and their rationale. Changes to this document do not silently change product scope. Earlier research memos are background; this document takes precedence where stack recommendations differ.

## Locked stack

| Area | Decision |
| --- | --- |
| First platform | Native Linux/KDE desktop application |
| Compatibility baseline | [Kubuntu 26.04 LTS](https://kubuntu.org/news/kubuntu-26-04-release-notes/); reference release stack Qt 6.10.2, KDE Frameworks 6.24.0, Plasma 6.6 |
| Implementation language | C++ |
| Language/build floor | C++20; GCC 13.3 or a compatible compiler, with target-specific runtime dependencies |
| Interface | Qt 6, Qt Quick/QML, Qt Quick Controls, Kirigami |
| Desktop conventions | KDE Human Interface Guidelines, Breeze styling and icons, KDE desktop integration |
| Application controller | C++ and Qt models, properties, signals and slots |
| Merge engine | C++, independent of Qt UI objects and platform I/O |
| Git integration | Installed Git CLI through a constrained C++ adapter using asynchronous QProcess |
| Git compatibility floor | Git 2.43; test it as well as the version supplied by Kubuntu 26.04 |
| Saving and recovery | C++ and Qt file APIs |
| Build system | CMake, with KDE Extra CMake Modules where needed |

The application has no Rust bridge, Python runtime, embedded browser, or mandatory libgit2 dependency. Use the named baseline rather than silently advancing to a later Kubuntu LTS. Support normal baseline updates while avoiding APIs introduced after its Qt/KF versions.

The development host reports Tuxedo OS 24.04.4 on the Ubuntu noble base, glibc 2.39, GCC 13.3 and Git 2.43.0. These observations establish a second required build/test target; Kubuntu 26.04 remains the selected KDE/API baseline. Produce separate artifacts for the two OS bases. Build the Tuxedo artifact against noble and its vendor Qt/KDE packages; build the Kubuntu artifact against Kubuntu 26.04. Neither artifact is assumed portable to the other base. Match actual runtime dependencies and verify installation on both systems before claiming compatibility.

The editor uses Qt Quick Controls TextArea in plain-text mode with a C++ QQuickTextDocument adapter. The diff algorithm/library and package format remain open. Qt Quick/Kirigami is the selected application UI; replacing it with a Widgets application would require revising this stack decision. A Qt Widgets dependency for desktop styling or application bootstrap does not by itself select a Widgets interface.

## Architecture and dependency boundaries

The [component diagram](reports/Branch%20Truce%20components.html) gives the overview. Dependencies flow from the interface through the controller into components with narrow contracts.

| Component | Owns | Boundary |
| --- | --- | --- |
| KDE interface | Windows, file list, comparison panes, editor presentation, controls and notices | Sends commands and renders controller state; performs no Git or file writes |
| Application controller | Session lifecycle, command dispatch, background jobs and operation progress | Coordinates the engine, Git adapter and persistence; exposes Qt-facing models |
| Merge engine | Source correspondence, line/region choices, result revisions, decision state and undo/redo | Accepts values and commands; returns new state, edits or typed errors |
| Git adapter | Repository discovery, source retrieval, index inspection and explicit staging | Offers named operations; does not accept arbitrary command strings |
| Save and recovery | App-owned draft checkpoints, result serialization, guarded worktree writes, recovery and restore validation | Returns explicit outcomes; Save writes only the app store; worktree publishing belongs to Mark resolved |

Keep QObject, QML, editor-widget types, subprocess handles and filesystem mechanisms outside the merge engine. Translate between domain positions and editor offsets in the interface adapter. Use explicit ownership, RAII and typed results rather than implicit global state.

A future desktop implementation can retain or adapt the domain contracts and supply its own native UI and platform adapters. Shared UI code is not required. Describe future technical designs through the same screens, components and contracts.

Initial source layout:

```text
src/
  app/          # Bootstrap, session controller, Qt-facing models
  core/         # Merge state, correspondence, commands and history
  git/          # Git CLI adapter and repository metadata
  storage/      # Result writer and recovery store
  ui/           # QML, editor adapter and KDE integration
tests/
  core/
  git/
  storage/
  ui/
```

These are proposed implementation directories, not existing code.

## Screens and interface components

| Surface | Components and implementation responsibility |
| --- | --- |
| Session workspace | Kirigami application window; file-list model and sidebar; selected-file state; progress and outstanding-work indicators |
| Comparison workspace | Read-only left and right sources around an editable center result; source identity headers; line/region action gutters; reading controls and scoped search |
| Earlier-version context | On-demand base view within the workspace; hiding it preserves editor state and selection |
| Review and handoff | Session summary with links to outstanding files; separate app-draft, worktree and staging revisions; stale-staged warnings block a successful finish |
| Recovery and errors | Restore/leave/discard interactions and file-specific error surfaces, without replacing a failed load with an apparent empty result |

These are logical surfaces, not a requirement for five separate windows or a sequence of startup pages. Launch directly into the session. The UI follows KDE theme, focus, dialog and scaling conventions. It includes mouse controls and basic editing shortcuts; the design does not promote second-iteration keyboard navigation into first-release scope.

### Editor boundary

Expose an editor adapter for loading read-only sources, applying result edits, mapping positions, rendering difference ranges, navigation, search and selection. Display alignment and whitespace markers must never become saved text.

Prototype unequal insertions, wrapped lines, repeated passages, word-level highlighting, line/region controls, Unicode and IME editing. Prove that undo restores both result text and contribution decisions, including Ignore without text changes. Test accepting the second side after editing the first, partial line choices from both sides, edits that delete an anchor, and the recovery-before-acknowledgment boundary described below.

Qt Quick Controls TextArea in plain-text mode is the selected text component, following the [editor experiment](docs/editor-integration.md). The product comparison uses immutable read-only sources around the editable result. Its C++ QQuickTextDocument adapter translates native input into revision-bound editing commands and uses application history and recovery instead of native undo. TextArea remains an editing foundation; alignment, highlighting, navigation and search require separate implementation. [KTextEditor exposes QWidget views](https://api.kde.org/ktexteditor-view.html) and cannot be assumed to drop into a Quick scene.

## Domain model and editing

Use immutable source snapshots and revisioned editable results. Capture the launch-time worktree bytes, file mode and identity before editing, separately from all stage inputs. The initial result is a lossless decoding of those worktree bytes, including existing manual edits, cleanly merged text and conflict markers. Do not initialize the entire result from the common ancestor or regenerate it from the two sides. Resume a validated app draft instead when one exists. If decoding or safe correspondence is unavailable, explain the limitation before enabling the affected operation.

A source choice replaces only its explicitly mapped result range. Recognized merge-marker ranges may be replaced by a confirmed choice; do not strip markers globally. Attach mappings using captured sources and verified correspondence, not marker text alone. Keep ambiguous ranges directly editable and disable automated choices whose target cannot be established. Existing source-like text must not be duplicated simply because its decision is still undecided. Source decisions describe the user's judgments, not proof that a byte sequence is absent or present.

| Record | Essential data |
| --- | --- |
| Repository context | Worktree root, actual Git directory, common Git directory, operation identity and session ID |
| Source snapshot | Opaque stage object ID and raw stored bytes; separately materialized worktree-form bytes, conversion policy and provenance |
| File session | Repository-relative byte path, launch-time worktree snapshot, source snapshots, correspondence map, result buffer and decisions |
| Revision state | Current result/decision revision, explicit Save checkpoint, reviewed revision, written worktree revision and verified staged snapshot |
| Recovery record | Schema version, operation identity, source/launch snapshots, latest committed state, explicit Save checkpoint and mutation outcomes |

Do not use one `resolved` boolean as the entire file state. Support, undecided contributions, unsaved edits, review validity, Git index state and failures are separate dimensions. Session totals derive from those dimensions.

[Git index stage 1](https://git-scm.com/docs/git-ls-files) supplies common earlier content when present; stages 2 and 3 supply the competing sources. Missing content is not an empty file. Carry operation-aware provenance to the UI so rebase labels do not blindly equate a Git side with the user's branch. Preserve opaque object IDs without assuming one hash length.

Commands address stable source anchors and a known result revision. Track each mapped result range with a stable hunk/contribution ID, half-open UTF-8 byte offsets at character boundaries, and explicit insertion affinity. Editor offsets are translated by the adapter. Transform anchors through every manual edit; reject ambiguous or stale mappings instead of applying to a nearby repeated passage. Keep the diff/correspondence algorithm behind a replaceable interface, with deterministic output on fixed fixtures.

One application history coordinates manual edits and choices. Accepting one side leaves the competing decision available; resolving using one side is a separate command. Ignore updates only an undecided contribution's state. Combined choices carry explicit ordering. Line picks, section picks, bulk actions and whole-file replacement record their affected scope and enough prior state for undo.

Accepting another side into a range that already contains accepted text proposes an explicit ordered combination of the selected contribution ranges. It must not silently replace the earlier accepted side. Any overlap with manual edits requires an exact replacement preview and confirmation. If the combination or its anchors cannot be established safely, require manual editing for that range. Store the selected order as decision metadata; do not infer it later from the edited result. Undo and redo move through the command stack only. To change an earlier accepted decision after later edits, undo those intervening commands or issue a new previewed replacement; never selectively erase the old contribution through newer edits.

Undo/redo restores text and decision state, not external Git or filesystem history. Keep in-session history through Save. Mark resolved is the only explicit review action; capture a reviewed revision when the user confirms it, even if a later write or staging step fails. Undo or any later text/decision change invalidates review of the current revision. Persisting the complete undo chain after reopening remains outside the initial commitment.

## Execution and concurrency

Qt owns the GUI event loop. Use workers for expensive correspondence and serialization work, and asynchronous QProcess signals for Git. Never wait synchronously for long work on the GUI thread. Update Qt-facing models on their owning thread.

Background requests carry a session generation and input revision. Discard stale results; cancellation cannot publish an obsolete model or successful mutation. Serialize app-owned mutations per worktree. Git's own locks remain authoritative; an application queue is not a lock against other programs.

Return typed outcomes for unreadable inputs, unsupported files, stale baselines, encoding problems, failed writes, process failures and uncertain mutations. Attach failures to the affected file and operation. Preserve editable buffers while failures are handled.

## Git integration

### Repository adapter

| Operation | Implementation direction |
| --- | --- |
| Discover repository | Use Git discovery such as `rev-parse`; do not assume `.git` is a directory or that the current directory is the root |
| Enumerate current conflicts | Combine NUL-delimited unmerged entries with `status --porcelain=v2 -z`; group paths and retain stage presence, modes and IDs |
| Read sources | Capture raw stage objects for identity, then materialize supported worktree conversions using `cat-file --filters --path=<path> <oid>` under the checked helper policy |
| Check a mutation baseline | Reinspect the relevant operation, index entries and destination against the session's expected state |
| Stage reviewed result | Use an exact literal path with `git --literal-pathspecs add -- <path>`, then verify the resulting index entry |

These examples describe fixed adapter operations, not shell command strings. Stage no unrelated paths. Never use `add --all`, commit, continue, reset or abort as an incidental consequence of resolving a file.

Use NUL-delimited machine formats and preserve filename bytes separately from display strings. Define and test conversion at Qt/path API boundaries; unsupported filenames must not become different writable paths. Test linked worktrees, subdirectory launches, spaces, newlines and leading dashes.

Supported text candidates have stages 2 and 3 at the same path, matching regular-file modes 100644 or 100755, and supported lossless conversions. Stage 1 may be absent for add/add; when present it must also be a supported regular-file source. Missing stage 2 or 3, incompatible modes, symlinks 120000, submodules 160000, binary content and ambiguous topology remain visible but unsupported. Preserve the agreed executable bit; do not choose between conflicting modes silently. [Porcelain v2 unmerged records](https://git-scm.com/docs/git-status/2.43.0) provide stage modes; unmerged index entries alone do not record rename history. A rename-specific conflict that requires topology decisions is unsupported, while ordinary text stages at a renamed path can be handled without claiming to classify its rename history.

### Source conversion and result format

Raw object bytes are identity data, not an assumed worktree encoding. Before reading editable sources, capture effective attributes/configuration and apply supported Git checkout conversions, including EOL, ident and working-tree-encoding, for the captured path. [Git cat-file filters](https://git-scm.com/docs/git-cat-file) materialize that form; textconv is a potentially lossy display transform and is not an editing source. The helper policy below is checked before materialization, not only before staging.

Decode materialized sources into the engine's UTF-8 representation while retaining original bytes and format metadata. Serialize the result according to explicit current worktree attributes when specified; otherwise preserve the launch-time result's encoding, BOM and newline policy. Side-specific formats do not override this result policy. Mixed/contradictory metadata, unsupported encoding conversions or a format change that cannot round-trip must block publishing and explain the issue. Validate clean/checkout round trips and staged canonical bytes; do not assume a stored blob equals saved worktree bytes.

### Operation and source identity

Discover state under the actual per-worktree Git directory, not a guessed `.git` path. A supported merge has a verified single MERGE_HEAD plus HEAD. A supported rebase uses rebase-merge or rebase-apply metadata verified as a rebase, with REBASE_HEAD or validated replay metadata identifying the commit being replayed; rebase-apply used for `git am` is not a supported rebase.

For a merge, label stage 2 as the current checkout contribution and stage 3 with the verified merged commit/ref. For a rebase, stage 2 is the current rebased state, including already replayed commits, and stage 3 is the commit being replayed. Use recorded head-name/orig-head/onto only as context after validating available metadata. Always retain the exact per-file stage IDs as provenance. Missing names produce honest commit/stage descriptions, not invented branch names.

CHERRY_PICK_HEAD, REVERT_HEAD, apply-mailbox state, or unresolved entries without a verified supported merge/rebase produce an unsupported-operation session. Stash/checkout conflicts without a reliable operation identity also remain unsupported. Show the limitation and current files, but disable editing/publishing rather than relabeling them as merge or rebase work. Conflicting or corrupt operation metadata is an explicit failure.

### Launch and mergetool compatibility

The controller owns one session containing the conflicts discovered for the launched work. Both entry points use the same draft store and Save semantics. Repository-session mode owns staging through the Git adapter. Mergetool mode has a small blocking launcher connected to the GUI session, with the parent Git process owning staging of its currently launched path. The launcher and GUI lifetime are separate so the GUI can observe parent acknowledgment and remain available for the all-files session.

Conventional [git mergetool](https://git-scm.com/docs/git-mergetool) passes per-file inputs including BASE, LOCAL, REMOTE and MERGED, and interprets completion through its parent protocol. Parent staging, backup restoration, temporary-file lifetime and iteration over multiple files can conflict with draft saves and an all-files session.

For normal text conflicts, [Git 2.43's mergetool parent](https://raw.githubusercontent.com/git/git/v2.43.0/git-mergetool.sh) stages MERGED after success and restores its backup after failure. Therefore Save never writes MERGED. Copy needed temporary inputs and the initial MERGED snapshot into app-owned storage. Use a registered configuration with trustExitCode enabled and keep the launcher blocked until the user confirms Mark resolved or explicitly leaves.

After Mark resolved prepares and writes the launched path, the launcher returns success so the parent can stage it. The GUI keeps that path pending until index inspection verifies the expected canonical result; only then is it resolved. Parent staging failure remains "written, unresolved" with a retry after ownership is released. Do not stage the launched path in both app and parent. On leave, cancellation or a failed write, the launcher returns non-zero; the parent may restore the original MERGED, while all committed app edits and explicit Save checkpoints survive independently.

Invocations join a session keyed by worktree and operation identity. At most one parent owns a path at a time. Other files remain editable/saveable in the all-files session, but a resolve request waits until that path has a parent invocation or is explicitly assigned app-owned staging after the active parent session ends. Indicate the waiting state rather than returning success for an unlaunched path. A user may close with drafts preserved, but closing does not release unresolved launchers with success. Unexpected termination or unverifiable acknowledgment is an uncertain operation requiring reinspection. Final flags, local communication transport, session teardown and retry mechanics remain prototype details.

This is an implementation gate. Do not advertise mergetool compatibility or provide a final configuration recipe until fixture tests demonstrate the complete parent/app handoff. Git merge-tool compatibility remains required; the gate blocks declaring it implemented.

### Process and trust policy

Use [QProcess](https://doc.qt.io/qt-6.10/qprocess.html) with a selected absolute Git executable, separate arguments, explicit working directory and controlled environment. Allow only adapter-defined operations. Bound execution and output, drain stdout/stderr, supervise cancellation and inspect state after interrupted mutations. Inherit no arbitrary Git-directory/index overrides without validating them against the selected repository.

Disable unnecessary pagers, external diff/textconv, interactive prompts and implicit network retrieval for app-owned inspection. Preserve Git's repository ownership checks. Shell-free execution prevents shell interpretation of arguments; it is not a sandbox for Git itself.

Git configuration can invoke [filters](https://git-scm.com/docs/gitattributes) or [hooks](https://git-scm.com/docs/githooks) during ordinary operations. The first release executes no repository-configured external conversion programs: files requiring a custom clean/smudge/process filter remain unsupported before materialization. Built-in supported conversions still apply. Check applicable executable hooks, configured hook paths and other helper settings before staging or releasing a mergetool parent to stage; if the operation would run an unapproved helper, keep it blocked rather than silently disabling repository behavior. A future helper allowlist requires a separate explicit policy.

Disable external filesystem-monitor commands for app-owned inspection and reject parent-owned staging that would use an external monitor. Exclude partial/promisor repositories from first-release editing rather than relying on a newer Git-only lazy-fetch control to enforce offline behavior on Git 2.43. Missing local objects and unsupported helper requirements remain outstanding. Recheck relevant configuration before mutations. The app performs no fetch/pull/push or background network work.

An independently invoked mergetool parent may already have performed checkout conversions before starting the app launcher. The app cannot prevent that earlier helper execution or sandbox its caller. Reject unsupported helper-dependent editing when discovered, and describe this external trust boundary honestly. Checking before launcher success protects the subsequent staging handoff, not work the parent already performed. Test the registered configuration and supported repository policy together.

## Saving and Mark resolved

Save creates an explicit checkpoint of the current result and decisions in the app store. It does not write the worktree, MERGED or Git index in either launch mode. The UI says "Draft saved in Branch Truce; repository file unchanged." Automatic recovery records and explicit Save checkpoints are distinct: recovery of later edits does not move the last manually saved checkpoint. Discard unsaved changes restores that checkpoint, or the captured initial result if none exists.

Only Mark resolved publishes a result. Preserve the selected result-format policy, final-newline behavior and agreed executable permissions. Reading aids do not alter serialization. Known remaining merge-marker ranges block publishing until explicitly resolved; do not delete marker-like source text globally or treat absence of marker text as proof of review. When correspondence is ambiguous, require full-file review and explicit confirmation rather than inventing resolved decisions.

The Linux storage writer uses pinned directory/file descriptors and atomic exchange, with no direct-write fallback. It validates the destination's exact path bytes, identity, content and permissions before replacement and inspects both the published and displaced entries afterward. See [destination publication](docs/destination-publication.md) for its policy, race evidence, retained-entry handling and caller requirements. Publication and Git staging remain separate operations.

Mark resolved coordinates these steps:

1. Capture the exact result/decision revision confirmed by Mark resolved, including the additional confirmation for undecided source choices. Cancel without changing text or decisions. Persist that review declaration.
2. Create the explicit app Save checkpoint. On failure, retain the current result and do not publish.
3. Validate operation, index, attributes/helper policy and launch-time or last-written destination baseline; persist mutation intent.
4. Serialize and write that revision to the worktree. On failure, keep the saved app draft and do not stage.
5. Stage its exact path in repository-session mode, or await verified parent staging in mergetool mode. Verify canonical content and modes under the conversion policy.
6. Record written and staged revisions only when each outcome is verified. Retain the reviewed declaration even if a later operation fails; it is not a resolution acknowledgment.

Distinguish "draft saved, repository unchanged", "written, unresolved" and "written and resolved". A process timeout is an uncertain outcome until inspected, not proof that no write occurred. Do not roll back unrelated user or Git work to simulate atomicity. This also defines the reviewed-but-unresolved state without adding a separate review action.

Later edits or undo invalidate review of the current result. Preserve the earlier written/staged snapshot and label the newer app draft "Earlier result staged; current draft not applied." Treat this as an outstanding handoff blocker, even when Git no longer lists a conflict. A successful finish requires explicit Mark resolved for the new revision or deliberate discard back to the verified staged result. Explicit leave after a warning remains possible without a success claim. Do not automatically reconstruct conflict stages or imply that Git staged content has followed every editor change.

Mutation-time stale-state checks prevent overwriting unexpected work. They do not add automatic outside-app comparison/reconciliation to the first release. A mismatch blocks the mutation, keeps buffers, and explains the failure.

## Recovery persistence

Use an app-owned per-session store under a standard Qt application-data location, independent of the repository and mergetool temporary files. The [version 1 recovery journal](docs/recovery-journal.md) defines the implemented metadata, payload encoding, integrity checks and acknowledgment boundary using Qt file APIs.

Persist source/launch snapshots before accepting edits. An editing transaction is an atomic domain command: a committed text insertion/deletion, committed IME input, source choice, undo or redo. Append a versioned, integrity-checked recovery transaction and make its referenced payloads available before acknowledging the command as completed in the UI. Native editor mutations cannot bypass this boundary. Acknowledge each completed transaction; history grouping for convenient undo does not delay recovery. IME pre-edit and queued, visibly pending input are not yet completed commands.

Use an ordered journal with complete-record checks and atomic checkpoints; ignore an incomplete trailing record after a crash, never an acknowledged transaction. Record explicit Save checkpoints separately so recovery retains both the current dirty state and its last manual-save baseline. A persistence failure blocks further acknowledgments, exposes pending input and preserves the existing buffer rather than silently accepting unrecoverable edits. Use user-only access and avoid logging file bodies.

Ordinary file switching and closing wait for queued editing transactions to settle or require explicit discard of pending input. A crash may interrupt pending input, but a normal close must not silently drop it.

Validate schema, payload integrity, worktree/operation identity and current disk/index state before restore or subsequent writes. Unknown, corrupt or stale records must not be applied silently. Restore unsaved edits as unsaved and preserve unfinished decisions. A discard must durably prevent the discarded state from becoming the current restored result.

Application-crash recovery has zero acknowledged-transaction loss as its correctness target. Test termination before and after journal acknowledgment, checkpoint replacement, explicit Save, publication and durable discard. Pending input may need repeating; acknowledged unsaved edits must restore as not manually saved. This is not a machine-power-loss guarantee, and QSaveFile alone is not the journal protocol. Measure acknowledgment latency and set resource limits before implementation is accepted. Full reopened undo history is not required.

## Build, packaging and verification

Build the domain core as an independently testable C++20 target. Separate Qt-facing application, adapter and QML targets in CMake. Declare dependencies explicitly; avoid adding a second language/build system for application logic. Validate the GCC 13.3-compatible build and Git 2.43 API floor on the noble/Tuxedo target; also build and test natively on Kubuntu 26.04. Resolve exact Qt/KDE build packages and runtime ABI dependencies separately for each target. Do not ship a Kubuntu-built binary as the Tuxedo artifact without separate compatibility evidence.

Deploy the needed Qt/QML modules, Kirigami, KDE styles, icons and platform integration. Keep any Widgets bootstrap dependency explicit. Use native KDE desktop-file and application metadata conventions. No package format is locked; a native `.deb` is the initial candidate. Flatpak would need separate design for Git executable selection, repository access, worktree metadata and host helpers.

Use CTest and Qt testing facilities where applicable. Validate:

- Core commands, transformed/ambiguous anchors, accept-after-edit, ordered partial combinations, revision guards and history-stack undo of text and decisions.
- Real temporary Git repositories for merge/rebase identities, rejected other operations, missing stages, add/add, mode/type disputes, unrelated staged files, built-in conversions, rejected helpers, odd paths and worktrees.
- Git 2.43 and target Git parent processes for success, app-only draft Save, cancellation/backup restoration, parent acknowledgment, repeated invocations, multi-file waiting and process failure.
- Preservation of launch-time manual/rerere results; no worktree mutation by Save; stale staged content blocking success after editing; explicit discard and leave behavior.
- Conversion fixtures for CRLF/LF, working-tree-encoding, differing side formats, BOM/final newline, unsupported conversions and verified staged canonical content.
- Persistence failures, partial Mark resolved, stale destinations/indexes, termination at every transaction/checkpoint boundary, corruption and durable discard.
- The packaged application on the Kubuntu baseline and actual Tuxedo OS, including Wayland, scaling, KDE themes, basic editor input and accessibility metadata.

Use compiler warnings and sanitizer builds to check C++ ownership and undefined behavior. Measure representative files and long lines before choosing memory, latency, output or size limits. Exceeding a supported limit must produce an explicit failure rather than truncated work. No implementation or performance claims follow from this document.

## Open decisions and validation gates

| Decision | Evidence needed |
| --- | --- |
| Remaining editor capabilities on the selected TextArea foundation | Validate alignment, wrapping, navigation, search and input on target desktops |
| Diff/correspondence implementation | Difficult-text fixtures, correctness review and representative performance measurements |
| Mergetool transport and lifecycle | Implement and test the specified app-only drafts, parent-owned launched-path staging, multi-file waiting and verified acknowledgments |
| Text formats, path support and limits | Lossless round-trip fixtures, Qt boundary tests and measured resource budgets |
| Git/helper-policy enforcement | Git 2.43 and target Git fixtures for built-in conversions, denied custom helpers and offline repository restrictions |
| Recovery layout and acknowledgment latency | Crash/fault tests proving zero acknowledged-transaction loss, complete records and durable discard |
| Target-specific packaging | Clean noble/Tuxedo and Kubuntu builds, dependency manifests and real runtime/install validation |

Resolve these details within the locked stack. Any required change to the stack itself must be an explicit technical-spec decision.

## Technical references

- [Kubuntu 26.04 release baseline](https://kubuntu.org/news/kubuntu-26-04-release-notes/)
- [KDE application design guidance](https://develop.kde.org/hig/kde_app_design/) and [Kirigami C++ setup](https://develop.kde.org/docs/getting-started/kirigami/setup-cpp/)
- [QML and C++ integration](https://doc.qt.io/qt-6.10/qtqml-cppintegration-overview.html)
- [QProcess](https://doc.qt.io/qt-6.10/qprocess.html) and [QSaveFile](https://doc.qt.io/qt-6.10/qsavefile.html)
- [KTextEditor view API](https://api.kde.org/ktexteditor-view.html)
- [Git unmerged index entries](https://git-scm.com/docs/git-ls-files), [Git mergetool protocol](https://git-scm.com/docs/git-mergetool), and [Git attributes/conversions](https://git-scm.com/docs/gitattributes)
