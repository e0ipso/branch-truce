---
okf_version: '0.1'
---
# kenkeep Index

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
_None._

## Conventions (how we build)
- Open [**Keep failed work outstanding**](practice-keep-failed-work-outstanding.md) to learn about: Keep failed saves and resolutions visible, retain edited results, and claim resolution only after both steps succeed. #product #failures #saving #resolution
- Open [**Keep unsupported and unfinished work visible**](practice-keep-unsupported-and-unfinished-work-visible.md) to learn about: Show all current files, prominent outside-app warnings, and an honest handoff that directs users to outstanding work. #product #scope #progress #handoff
- Open [**Design desktop apps around screens and components**](practice-design-desktop-apps-around-screens-and-components.md) to learn about: Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit. #architecture #desktop #techspec
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) to learn about: Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation. #architecture #concurrency #cancellation #git #desktop
- Open [**Keep sources identifiable and the result editable**](practice-keep-sources-identifiable-and-the-result-editable.md) to learn about: Require truthful source identities, an editable center result, earlier context, and reading aids that preserve text. #product #comparison #editing
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) to learn about: Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release. #packaging #qml #qt #abi #debian
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) to learn about: Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history. #session #snapshots #editing #source-identity
- Open [**Make conflict choices explicit and reversible**](practice-make-conflict-choices-explicit-and-reversible.md) to learn about: Keep accept, ignore, combine, and broader choices distinct, inspectable, and reversible with their decision states. #product #choices #editing #undo
- Open [**Recover unfinished work without changing its status**](practice-recover-unfinished-work-without-changing-its-status.md) to learn about: Restore unsaved edits and outstanding decisions after closing or a crash while preserving honest work status. #product #recovery #editing #progress
- Open [**Separate Save from Mark resolved**](practice-separate-save-from-mark-resolved.md) to learn about: Save keeps app-only drafts; Mark resolved writes the reviewed result and resolves the file in Git. #product #saving #resolution #review
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) to learn about: Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline. #mergetool #publication #git #failures
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) to learn about: Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget. #git #conversion #limits #failures
- Open [**Keep PRD.md focused on user needs**](practice-keep-prd-focused-on-user-needs.md) to learn about: PRD.md describes user needs, interactions, and capabilities without technical or implementation details. #product #prd #documentation
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) to learn about: TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable. #documentation #techspec #prd
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) to learn about: Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying. #publication #storage #concurrency #git
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) to learn about: In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles. #testing #qml #ui #offscreen
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) to learn about: docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision. #ui #design #kde #kirigami #qml
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) to learn about: GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard. #testing #git #concurrency #mergetool
- Open [**Keep repository documentation about current state**](practice-keep-repository-documentation-about-current-state.md) to learn about: Describe current behavior and durable conventions; keep historical run reports and implementation narratives out of repository docs. #documentation #conventions
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) to learn about: src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites. #qml #lint #ui #ci
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) to learn about: Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files. #git #commits #ci #conventions #release

## Components (what exists)
- Open [**Branch Truce desktop implementation baseline**](map-branch-truce-desktop-implementation-baseline.md) to learn about: Locked C++/Qt Quick/Kirigami stack on Kubuntu 26.04; native release packages also target Fedora 44 and pinned Arch Linux. #architecture #desktop #platform #cpp
- Open [**Mergetool session socket and real Git parent**](map-mergetool-session-socket-and-real-git-parent.md) to learn about: The session socket sits under XDG_RUNTIME_DIR or /tmp/branch-truce-<uid>; real-parent tests exercise staging and launcher handoff. #mergetool #git #transport #compatibility
- Open [**Branch Truce first-release scope**](map-branch-truce-first-release-scope.md) to learn about: PRD.md defines a standalone, offline Linux app for text conflicts during merges and rebases. #product #scope #workflow
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) to learn about: ctest acceptance.journeys drives the real Main scene offscreen over real Git merges and rebases with private XDG dirs and an offline git guard. #testing #acceptance #qml #offscreen #git
- Open [**Per-base Debian artifacts and container inspection**](map-per-base-debian-artifacts-and-container-inspection.md) to learn about: Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch release packages, each validated in a clean native target. #packaging #platform #debian #containers #compatibility
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) to learn about: validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE. #packaging #platform #validation #wayland #accessibility #compatibility
- Open [**Recovery journal and acknowledgment boundary**](map-recovery-journal-and-acknowledgment-boundary.md) to learn about: Persist complete text-and-decision transactions before acknowledgment; retain failed input and loaded captures for retry. #recovery #storage #editing #failures
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) to learn about: ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits. #git #classification #conversion #startup #scope
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) to learn about: Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures. #git #discovery #source-identity #scope
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) to learn about: main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it. #ui #startup #kde #icons #testing #preview
- Open [**Revision-bound passage correspondence**](map-revision-bound-passage-correspondence.md) to learn about: Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear. #comparison #editing #revisions #failures

## By topic

### #git
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) — Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline.
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
### #product
- Open [**Keep sources identifiable and the result editable**](practice-keep-sources-identifiable-and-the-result-editable.md) — Require truthful source identities, an editable center result, earlier context, and reading aids that preserve text.
- Open [**Recover unfinished work without changing its status**](practice-recover-unfinished-work-without-changing-its-status.md) — Restore unsaved edits and outstanding decisions after closing or a crash while preserving honest work status.
- Open [**Keep failed work outstanding**](practice-keep-failed-work-outstanding.md) — Keep failed saves and resolutions visible, retain edited results, and claim resolution only after both steps succeed.
### #editing
- Open [**Keep sources identifiable and the result editable**](practice-keep-sources-identifiable-and-the-result-editable.md) — Require truthful source identities, an editable center result, earlier context, and reading aids that preserve text.
- Open [**Recover unfinished work without changing its status**](practice-recover-unfinished-work-without-changing-its-status.md) — Restore unsaved edits and outstanding decisions after closing or a crash while preserving honest work status.
- Open [**Make conflict choices explicit and reversible**](practice-make-conflict-choices-explicit-and-reversible.md) — Keep accept, ignore, combine, and broader choices distinct, inspectable, and reversible with their decision states.
### #failures
- Open [**Recovery journal and acknowledgment boundary**](map-recovery-journal-and-acknowledgment-boundary.md) — Persist complete text-and-decision transactions before acknowledgment; retain failed input and loaded captures for retry.
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) — Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline.
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) — Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget.
### #qml
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) — ctest acceptance.journeys drives the real Main scene offscreen over real Git merges and rebases with private XDG dirs and an offline git guard.
### #scope
- Open [**Branch Truce first-release scope**](map-branch-truce-first-release-scope.md) — PRD.md defines a standalone, offline Linux app for text conflicts during merges and rebases.
- Open [**Keep unsupported and unfinished work visible**](practice-keep-unsupported-and-unfinished-work-visible.md) — Show all current files, prominent outside-app warnings, and an honest handoff that directs users to outstanding work.
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #testing
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) — ctest acceptance.journeys drives the real Main scene offscreen over real Git merges and rebases with private XDG dirs and an offline git guard.
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
### #ui
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
### #architecture
- Open [**Design desktop apps around screens and components**](practice-design-desktop-apps-around-screens-and-components.md) — Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit.
- Open [**Branch Truce desktop implementation baseline**](map-branch-truce-desktop-implementation-baseline.md) — Locked C++/Qt Quick/Kirigami stack on Kubuntu 26.04; native release packages also target Fedora 44 and pinned Arch Linux.
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #compatibility
- Open [**Per-base Debian artifacts and container inspection**](map-per-base-debian-artifacts-and-container-inspection.md) — Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch release packages, each validated in a clean native target.
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) — validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE.
- Open [**Mergetool session socket and real Git parent**](map-mergetool-session-socket-and-real-git-parent.md) — The session socket sits under XDG_RUNTIME_DIR or /tmp/branch-truce-<uid>; real-parent tests exercise staging and launcher handoff.
### #concurrency
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #desktop
- Open [**Design desktop apps around screens and components**](practice-design-desktop-apps-around-screens-and-components.md) — Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit.
- Open [**Branch Truce desktop implementation baseline**](map-branch-truce-desktop-implementation-baseline.md) — Locked C++/Qt Quick/Kirigami stack on Kubuntu 26.04; native release packages also target Fedora 44 and pinned Arch Linux.
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #documentation
- Open [**Keep PRD.md focused on user needs**](practice-keep-prd-focused-on-user-needs.md) — PRD.md describes user needs, interactions, and capabilities without technical or implementation details.
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.
- Open [**Keep repository documentation about current state**](practice-keep-repository-documentation-about-current-state.md) — Describe current behavior and durable conventions; keep historical run reports and implementation narratives out of repository docs.
### #mergetool
- Open [**Mergetool session socket and real Git parent**](map-mergetool-session-socket-and-real-git-parent.md) — The session socket sits under XDG_RUNTIME_DIR or /tmp/branch-truce-<uid>; real-parent tests exercise staging and launcher handoff.
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) — Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline.
- Open [**Hold a Git call to test racing repository changes**](practice-hold-a-git-call-to-test-racing-repository-changes.md) — GitGate holds one git subcommand on one path so a write lands mid-inspection; keep a race test only if it fails without its guard.
### #packaging
- Open [**Per-base Debian artifacts and container inspection**](map-per-base-debian-artifacts-and-container-inspection.md) — Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch release packages, each validated in a clean native target.
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) — validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE.
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) — Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release.
### #platform
- Open [**Per-base Debian artifacts and container inspection**](map-per-base-debian-artifacts-and-container-inspection.md) — Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch release packages, each validated in a clean native target.
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) — validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE.
- Open [**Branch Truce desktop implementation baseline**](map-branch-truce-desktop-implementation-baseline.md) — Locked C++/Qt Quick/Kirigami stack on Kubuntu 26.04; native release packages also target Fedora 44 and pinned Arch Linux.
### #ci
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #comparison
- Open [**Keep sources identifiable and the result editable**](practice-keep-sources-identifiable-and-the-result-editable.md) — Require truthful source identities, an editable center result, earlier context, and reading aids that preserve text.
- Open [**Revision-bound passage correspondence**](map-revision-bound-passage-correspondence.md) — Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear.
### #conventions
- Open [**Keep repository documentation about current state**](practice-keep-repository-documentation-about-current-state.md) — Describe current behavior and durable conventions; keep historical run reports and implementation narratives out of repository docs.
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #conversion
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) — Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget.
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.
### #debian
- Open [**Per-base Debian artifacts and container inspection**](map-per-base-debian-artifacts-and-container-inspection.md) — Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch release packages, each validated in a clean native target.
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) — Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release.
### #kde
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
### #offscreen
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) — ctest acceptance.journeys drives the real Main scene offscreen over real Git merges and rebases with private XDG dirs and an offline git guard.
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
### #prd
- Open [**Keep PRD.md focused on user needs**](practice-keep-prd-focused-on-user-needs.md) — PRD.md describes user needs, interactions, and capabilities without technical or implementation details.
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.
### #progress
- Open [**Keep unsupported and unfinished work visible**](practice-keep-unsupported-and-unfinished-work-visible.md) — Show all current files, prominent outside-app warnings, and an honest handoff that directs users to outstanding work.
- Open [**Recover unfinished work without changing its status**](practice-recover-unfinished-work-without-changing-its-status.md) — Restore unsaved edits and outstanding decisions after closing or a crash while preserving honest work status.
### #publication
- Open [**Treat mergetool file state as live evidence**](practice-treat-mergetool-file-state-as-live-evidence.md) — Check live MERGED bytes before trusting a recorded write, shrink restores after flushing, and wait for the socket on a wall-clock deadline.
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
### #recovery
- Open [**Recover unfinished work without changing its status**](practice-recover-unfinished-work-without-changing-its-status.md) — Restore unsaved edits and outstanding decisions after closing or a crash while preserving honest work status.
- Open [**Recovery journal and acknowledgment boundary**](map-recovery-journal-and-acknowledgment-boundary.md) — Persist complete text-and-decision transactions before acknowledgment; retain failed input and loaded captures for retry.
### #resolution
- Open [**Keep failed work outstanding**](practice-keep-failed-work-outstanding.md) — Keep failed saves and resolutions visible, retain edited results, and claim resolution only after both steps succeed.
- Open [**Separate Save from Mark resolved**](practice-separate-save-from-mark-resolved.md) — Save keeps app-only drafts; Mark resolved writes the reviewed result and resolves the file in Git.
### #saving
- Open [**Keep failed work outstanding**](practice-keep-failed-work-outstanding.md) — Keep failed saves and resolutions visible, retain edited results, and claim resolution only after both steps succeed.
- Open [**Separate Save from Mark resolved**](practice-separate-save-from-mark-resolved.md) — Save keeps app-only drafts; Mark resolved writes the reviewed result and resolves the file in Git.
### #source-identity
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #startup
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
### #storage
- Open [**Recovery journal and acknowledgment boundary**](map-recovery-journal-and-acknowledgment-boundary.md) — Persist complete text-and-decision transactions before acknowledgment; retain failed input and loaded captures for retry.
- Open [**Reconcile uncertain destination publication**](practice-reconcile-uncertain-destination-publication.md) — Use captured destination identities, fail closed on unsupported metadata, and inspect uncertain exchanges before retrying.
### #techspec
- Open [**Design desktop apps around screens and components**](practice-design-desktop-apps-around-screens-and-components.md) — Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit.
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.
### #abi
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) — Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release.
### #acceptance
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) — ctest acceptance.journeys drives the real Main scene offscreen over real Git merges and rebases with private XDG dirs and an offline git guard.
### #accessibility
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) — validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE.
### #cancellation
- Open [**Fence session work and serialize worktree mutations**](practice-fence-session-work-and-serialize-worktree-mutations.md) — Fence background completions by session and revision; serialize worktree mutations through cancellation reconciliation.
### #choices
- Open [**Make conflict choices explicit and reversible**](practice-make-conflict-choices-explicit-and-reversible.md) — Keep accept, ignore, combine, and broader choices distinct, inspectable, and reversible with their decision states.
### #classification
- Open [**Conflict admission and offline startup**](map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.
### #commits
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #containers
- Open [**Per-base Debian artifacts and container inspection**](map-per-base-debian-artifacts-and-container-inspection.md) — Developer per-base DEBs are separate from Kubuntu DEB, Fedora RPM and Arch release packages, each validated in a clean native target.
### #cpp
- Open [**Branch Truce desktop implementation baseline**](map-branch-truce-desktop-implementation-baseline.md) — Locked C++/Qt Quick/Kirigami stack on Kubuntu 26.04; native release packages also target Fedora 44 and pinned Arch Linux.
### #design
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
### #discovery
- Open [**Repository discovery and verified source identities**](map-repository-discovery-and-verified-source-identities.md) — Verify merge/rebase source identities and keep renamed text conflicts supported while isolating path provenance failures.
### #handoff
- Open [**Keep unsupported and unfinished work visible**](practice-keep-unsupported-and-unfinished-work-visible.md) — Show all current files, prominent outside-app warnings, and an honest handoff that directs users to outstanding work.
### #icons
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
### #kirigami
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
### #limits
- Open [**Bound each converted representation independently**](practice-bound-each-converted-representation-independently.md) — Reject cleaned canonical objects over 4194304 bytes before reading; give each Git phase its own 4194304-byte budget.
### #lint
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
### #preview
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
### #qt
- Open [**Compile QML to bytecode to avoid private Qt ABI pins**](practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md) — Keep qmlcachegen --only-bytecode; QML compiled to C++ links Qt_6_PRIVATE_API and pins the package to one exact Qt release.
### #release
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #review
- Open [**Separate Save from Mark resolved**](practice-separate-save-from-mark-resolved.md) — Save keeps app-only drafts; Mark resolved writes the reviewed result and resolves the file in Git.
### #revisions
- Open [**Revision-bound passage correspondence**](map-revision-bound-passage-correspondence.md) — Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear.
### #session
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
### #snapshots
- Open [**Keep session captures immutable across revisions**](practice-keep-session-captures-immutable-across-revisions.md) — Retain source bytes, conversion policy, provenance and the launch result separately from editable revision history.
### #transport
- Open [**Mergetool session socket and real Git parent**](map-mergetool-session-socket-and-real-git-parent.md) — The session socket sits under XDG_RUNTIME_DIR or /tmp/branch-truce-<uid>; real-parent tests exercise staging and launcher handoff.
### #undo
- Open [**Make conflict choices explicit and reversible**](practice-make-conflict-choices-explicit-and-reversible.md) — Keep accept, ignore, combine, and broader choices distinct, inspectable, and reversible with their decision states.
### #validation
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) — validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE.
### #wayland
- Open [**Installed-artifact validation in base containers**](map-installed-artifact-validation-in-base-containers.md) — validate-installed-artifact.sh runs each installed .deb in a container of its base on headless sway; real-desktop rows stay UNAVAILABLE.
### #workflow
- Open [**Branch Truce first-release scope**](map-branch-truce-first-release-scope.md) — PRD.md defines a standalone, offline Linux app for text conflicts during merges and rebases.
