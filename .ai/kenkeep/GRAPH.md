---
schema_version: 3
nodes_hash: 'sha256:490b3b8d76fe59b673ca7b995acff58de2941c478452b7f43d07fc8249b4afd9'
node_count: 50
---
# kenkeep Graph

Total nodes: 50

## map-branch-truce-first-release-scope

- **kind:** map
- **title:** Branch Truce first-release scope
- **path:** product/map-branch-truce-first-release-scope.md
- **tags:** product, scope, workflow
- **derived_from:** PRD.md

## map-conflict-admission-and-offline-startup

- **kind:** map
- **title:** Conflict admission and offline startup
- **path:** git/map-conflict-admission-and-offline-startup.md
- **tags:** git, classification, conversion, startup, scope
- **relates_to:** map-repository-discovery-and-verified-source-identities, practice-keep-unsupported-and-unfinished-work-visible, practice-keep-sources-identifiable-and-the-result-editable, practice-bound-each-converted-representation-independently

## map-conflict-marker-scan

- **kind:** map
- **title:** Conflict marker scan
- **path:** core/map-conflict-marker-scan.md
- **tags:** git, editing, markers, recovery
- **relates_to:** map-revision-bound-passage-correspondence, map-conflict-marker-sections, practice-keep-failed-work-outstanding
- **derived_from:** 56ab09b6-0c56-46ab-b8f0-695ab329cc97:map:0

## map-conflict-marker-sections

- **kind:** map
- **title:** Conflict marker sections
- **path:** core/map-conflict-marker-sections.md
- **tags:** git, editing, markers, undo, recovery
- **relates_to:** map-conflict-marker-scan, map-revision-bound-passage-correspondence, practice-make-conflict-choices-explicit-and-reversible, practice-keep-session-captures-immutable-across-revisions
- **derived_from:** 1d4a909e-497a-4b0c-a66a-f79616a21068:map:0

## map-desktop-comparison-and-workflow-integration

- **kind:** map
- **title:** Desktop comparison and workflow integration
- **path:** architecture/desktop-baseline/map-desktop-comparison-and-workflow-integration.md
- **tags:** architecture, desktop, platform, cpp
- **relates_to:** practice-design-desktop-apps-around-screens-and-components, map-branch-truce-first-release-scope, practice-treat-mergetool-file-state-as-live-evidence, map-mergetool-session-socket-and-real-git-parent, map-per-base-debian-artifacts-and-container-inspection, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
- **derived_from:** TECH_SPEC.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/Application.cpp, docs/editor-integration.md, docs/build.md, tests/fixtures/performance/evidence.md, src/core/ResourceLimits.h, docs/resource-limits.md, docs/mergetool-launcher.md, tests/app/ComparisonViewTests.cpp, tools/verify-install-prefix.sh, eafaf840-7389-45ad-aa53-003771d300c2:map:0

## map-desktop-platform-and-implementation-stack

- **kind:** map
- **title:** Desktop platform and implementation stack
- **path:** architecture/desktop-baseline/map-desktop-platform-and-implementation-stack.md
- **tags:** architecture, desktop, platform, cpp
- **relates_to:** practice-design-desktop-apps-around-screens-and-components, map-branch-truce-first-release-scope, practice-treat-mergetool-file-state-as-live-evidence, map-mergetool-session-socket-and-real-git-parent, map-per-base-debian-artifacts-and-container-inspection, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
- **derived_from:** TECH_SPEC.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/Application.cpp, docs/editor-integration.md, docs/build.md, tests/fixtures/performance/evidence.md, src/core/ResourceLimits.h, docs/resource-limits.md, docs/mergetool-launcher.md, tests/app/ComparisonViewTests.cpp, tools/verify-install-prefix.sh

## map-developer-debian-artifact-builds-and-inspection

- **kind:** map
- **title:** Native package build entry point
- **path:** packaging/artifact-paths/map-developer-debian-artifact-builds-and-inspection.md
- **tags:** packaging, platform, debian, containers, compatibility
- **relates_to:** map-branch-truce-desktop-implementation-baseline, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins, map-installed-artifact-validation-in-base-containers
- **derived_from:** dcc586bc-c490-4186-893e-e9ac032ec77c:map:4

## map-editing-and-recovery-resource-envelope

- **kind:** map
- **title:** Editing and recovery resource envelope
- **path:** architecture/desktop-baseline/map-editing-and-recovery-resource-envelope.md
- **tags:** architecture, desktop, platform, cpp
- **relates_to:** practice-design-desktop-apps-around-screens-and-components, map-branch-truce-first-release-scope, practice-treat-mergetool-file-state-as-live-evidence, map-mergetool-session-socket-and-real-git-parent, map-per-base-debian-artifacts-and-container-inspection, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
- **derived_from:** TECH_SPEC.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/Application.cpp, docs/editor-integration.md, docs/build.md, tests/fixtures/performance/evidence.md, src/core/ResourceLimits.h, docs/resource-limits.md, docs/mergetool-launcher.md, tests/app/ComparisonViewTests.cpp, tools/verify-install-prefix.sh, dcc586bc-c490-4186-893e-e9ac032ec77c:map:3

## map-installed-deb-validation-execution

- **kind:** map
- **title:** Installed DEB validation execution
- **path:** packaging/installed-validation/map-installed-deb-validation-execution.md
- **tags:** packaging, platform, validation, wayland, accessibility, compatibility
- **relates_to:** map-per-base-debian-artifacts-and-container-inspection, map-branch-truce-desktop-implementation-baseline, map-journey-acceptance-suite

## map-installed-desktop-application

- **kind:** map
- **title:** Installed desktop application
- **path:** architecture/desktop-baseline/map-installed-desktop-application.md
- **tags:** architecture, desktop, platform, cpp
- **relates_to:** practice-design-desktop-apps-around-screens-and-components, map-branch-truce-first-release-scope, practice-treat-mergetool-file-state-as-live-evidence, map-mergetool-session-socket-and-real-git-parent, map-per-base-debian-artifacts-and-container-inspection, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
- **derived_from:** TECH_SPEC.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/Application.cpp, docs/editor-integration.md, docs/build.md, tests/fixtures/performance/evidence.md, src/core/ResourceLimits.h, docs/resource-limits.md, docs/mergetool-launcher.md, tests/app/ComparisonViewTests.cpp, tools/verify-install-prefix.sh

## map-installed-validation-evidence-and-desktop-limits

- **kind:** map
- **title:** Installed validation evidence and desktop limits
- **path:** packaging/installed-validation/map-installed-validation-evidence-and-desktop-limits.md
- **tags:** packaging, platform, validation, wayland, accessibility, compatibility
- **relates_to:** map-per-base-debian-artifacts-and-container-inspection, map-branch-truce-desktop-implementation-baseline, map-journey-acceptance-suite

## map-installed-validation-tooling-regression-suite

- **kind:** map
- **title:** Installed validation tooling regression suite
- **path:** packaging/installed-validation/map-installed-validation-tooling-regression-suite.md
- **tags:** packaging, platform, validation, wayland, accessibility, compatibility
- **relates_to:** map-per-base-debian-artifacts-and-container-inspection, map-branch-truce-desktop-implementation-baseline, map-journey-acceptance-suite

## map-journey-acceptance-suite

- **kind:** map
- **title:** Journey acceptance suite
- **path:** ui/map-journey-acceptance-suite.md
- **tags:** testing, acceptance, qml, offscreen, git
- **relates_to:** practice-wait-for-layout-before-clicking-offscreen-dialog-items, map-branch-truce-first-release-scope, practice-make-conflict-choices-explicit-and-reversible, map-mergetool-session-socket-and-real-git-parent
- **derived_from:** eafaf840-7389-45ad-aa53-003771d300c2:map:4

## map-literal-project-text-in-native-controls

- **kind:** map
- **title:** Literal project text in native controls
- **path:** ui/map-literal-project-text-in-native-controls.md
- **tags:** ui, qml, paths, testing
- **relates_to:** practice-keep-unsupported-and-unfinished-work-visible, map-journey-acceptance-suite, map-startup-style-colour-scheme-and-icon-theme
- **derived_from:** eafaf840-7389-45ad-aa53-003771d300c2:map:2

## map-mergetool-launcher-decisions-and-reloads

- **kind:** map
- **title:** Mergetool launcher decisions and reloads
- **path:** git/mergetool/map-mergetool-launcher-decisions-and-reloads.md
- **tags:** mergetool, git, transport, compatibility
- **relates_to:** practice-treat-mergetool-file-state-as-live-evidence, map-branch-truce-desktop-implementation-baseline, practice-keep-unsupported-and-unfinished-work-visible, map-journey-acceptance-suite

## map-mergetool-socket-lookup-and-access

- **kind:** map
- **title:** Mergetool socket lookup and access
- **path:** git/mergetool/map-mergetool-socket-lookup-and-access.md
- **tags:** mergetool, git, transport, compatibility
- **relates_to:** practice-treat-mergetool-file-state-as-live-evidence, map-branch-truce-desktop-implementation-baseline, practice-keep-unsupported-and-unfinished-work-visible, map-journey-acceptance-suite

## map-native-release-package-artifact-flow

- **kind:** map
- **title:** Native release package artifact flow
- **path:** packaging/artifact-paths/map-native-release-package-artifact-flow.md
- **tags:** packaging, platform, debian, containers, compatibility
- **relates_to:** map-branch-truce-desktop-implementation-baseline, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins, map-installed-artifact-validation-in-base-containers

## map-real-git-parent-compatibility-and-fixture-isolation

- **kind:** map
- **title:** Real Git parent compatibility and fixture isolation
- **path:** git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md
- **tags:** mergetool, git, transport, compatibility
- **relates_to:** practice-treat-mergetool-file-state-as-live-evidence, map-branch-truce-desktop-implementation-baseline, practice-keep-unsupported-and-unfinished-work-visible, map-journey-acceptance-suite

## map-recovery-acknowledgment-and-retained-input

- **kind:** map
- **title:** Recovery acknowledgment and retained input
- **path:** recovery/journal/map-recovery-acknowledgment-and-retained-input.md
- **tags:** recovery, storage, editing, failures
- **relates_to:** practice-recover-unfinished-work-without-changing-its-status, practice-keep-session-captures-immutable-across-revisions, practice-keep-failed-work-outstanding, map-mergetool-session-socket-and-real-git-parent
- **derived_from:** docs/recovery-journal.md, src/recovery/RecoveryInventory.cpp, src/app/SessionModel.h, tests/fixtures/performance/evidence.md, src/storage/RecoveryStore.cpp, src/app/Application.cpp, src/app/main.cpp, src/ui/Main.qml, dcc586bc-c490-4186-893e-e9ac032ec77c:map:1, eafaf840-7389-45ad-aa53-003771d300c2:map:3

## map-recovery-journal-format-and-validation

- **kind:** map
- **title:** Recovery journal format and validation
- **path:** recovery/journal/map-recovery-journal-format-and-validation.md
- **tags:** recovery, storage, editing, failures
- **relates_to:** practice-recover-unfinished-work-without-changing-its-status, practice-keep-session-captures-immutable-across-revisions, practice-keep-failed-work-outstanding, map-mergetool-session-socket-and-real-git-parent
- **derived_from:** docs/recovery-journal.md, src/recovery/RecoveryInventory.cpp, src/app/SessionModel.h, tests/fixtures/performance/evidence.md, src/storage/RecoveryStore.cpp, src/app/Application.cpp, src/app/main.cpp, src/ui/Main.qml, dcc586bc-c490-4186-893e-e9ac032ec77c:map:0

## map-recovery-of-copied-launches-and-interrupted-publication

- **kind:** map
- **title:** Recovery of copied launches and interrupted publication
- **path:** recovery/journal/map-recovery-of-copied-launches-and-interrupted-publication.md
- **tags:** recovery, storage, editing, failures
- **relates_to:** practice-recover-unfinished-work-without-changing-its-status, practice-keep-session-captures-immutable-across-revisions, practice-keep-failed-work-outstanding, map-mergetool-session-socket-and-real-git-parent
- **derived_from:** docs/recovery-journal.md, src/recovery/RecoveryInventory.cpp, src/app/SessionModel.h, tests/fixtures/performance/evidence.md, src/storage/RecoveryStore.cpp, src/app/Application.cpp, src/app/main.cpp, src/ui/Main.qml, dcc586bc-c490-4186-893e-e9ac032ec77c:map:2

## map-recovery-store-selection-and-failure-scope

- **kind:** map
- **title:** Recovery store selection and failure scope
- **path:** recovery/journal/map-recovery-store-selection-and-failure-scope.md
- **tags:** recovery, storage, editing, failures
- **relates_to:** practice-recover-unfinished-work-without-changing-its-status, practice-keep-session-captures-immutable-across-revisions, practice-keep-failed-work-outstanding, map-mergetool-session-socket-and-real-git-parent
- **derived_from:** docs/recovery-journal.md, src/recovery/RecoveryInventory.cpp, src/app/SessionModel.h, tests/fixtures/performance/evidence.md, src/storage/RecoveryStore.cpp, src/app/Application.cpp, src/app/main.cpp, src/ui/Main.qml

## map-repository-discovery-and-verified-source-identities

- **kind:** map
- **title:** Repository discovery and verified source identities
- **path:** git/map-repository-discovery-and-verified-source-identities.md
- **tags:** git, discovery, source-identity, scope
- **relates_to:** practice-keep-sources-identifiable-and-the-result-editable, practice-keep-unsupported-and-unfinished-work-visible, map-branch-truce-first-release-scope

## map-result-conflict-marker-folding

- **kind:** map
- **title:** Result conflict-marker folding
- **path:** ui/map-result-conflict-marker-folding.md
- **tags:** comparison, qml, editing, markers
- **relates_to:** map-conflict-marker-scan, practice-keep-sources-identifiable-and-the-result-editable, practice-follow-the-ui-design-brief-for-the-kde-native-window
- **derived_from:** 3994e51f-d660-44e0-bca2-753952522f57:map:0

## map-revision-bound-passage-correspondence

- **kind:** map
- **title:** Revision-bound passage correspondence
- **path:** core/map-revision-bound-passage-correspondence.md
- **tags:** comparison, editing, revisions, failures
- **relates_to:** practice-keep-session-captures-immutable-across-revisions, practice-make-conflict-choices-explicit-and-reversible, practice-keep-failed-work-outstanding

## map-revision-bound-source-control-presentation

- **kind:** map
- **title:** Revision-bound source control presentation
- **path:** ui/map-revision-bound-source-control-presentation.md
- **tags:** comparison, qml, editing, revisions
- **relates_to:** practice-keep-sources-identifiable-and-the-result-editable, practice-make-conflict-choices-explicit-and-reversible, map-revision-bound-passage-correspondence
- **derived_from:** eafaf840-7389-45ad-aa53-003771d300c2:map:1

## map-runtime-qml-and-plugin-package-dependencies

- **kind:** map
- **title:** Runtime QML and plugin package dependencies
- **path:** packaging/artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md
- **tags:** packaging, platform, debian, containers, compatibility
- **relates_to:** map-branch-truce-desktop-implementation-baseline, practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins, map-installed-artifact-validation-in-base-containers

## map-startup-style-colour-scheme-and-icon-theme

- **kind:** map
- **title:** Startup style, colour scheme and icon theme
- **path:** ui/map-startup-style-colour-scheme-and-icon-theme.md
- **tags:** ui, startup, kde, icons, testing, preview
- **relates_to:** map-branch-truce-desktop-implementation-baseline, map-per-base-debian-artifacts-and-container-inspection, map-recovery-journal-and-acknowledgment-boundary, map-installed-artifact-validation-in-base-containers

## practice-bound-each-converted-representation-independently

- **kind:** practice
- **title:** Bound each converted representation independently
- **path:** git/practice-bound-each-converted-representation-independently.md
- **tags:** git, conversion, limits, failures
- **relates_to:** practice-keep-failed-work-outstanding

## practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins

- **kind:** practice
- **title:** Compile QML to bytecode to avoid private Qt ABI pins
- **path:** packaging/practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md
- **tags:** packaging, qml, qt, abi, debian
- **relates_to:** map-per-base-debian-artifacts-and-container-inspection, map-branch-truce-desktop-implementation-baseline

## practice-design-desktop-apps-around-screens-and-components

- **kind:** practice
- **title:** Design desktop apps around screens and components
- **path:** architecture/practice-design-desktop-apps-around-screens-and-components.md
- **tags:** architecture, desktop, techspec
- **relates_to:** map-branch-truce-first-release-scope, practice-keep-techspecs-and-prds-independent

## practice-fence-session-work-and-serialize-worktree-mutations

- **kind:** practice
- **title:** Fence session work and serialize worktree mutations
- **path:** architecture/practice-fence-session-work-and-serialize-worktree-mutations.md
- **tags:** architecture, concurrency, cancellation, git, desktop
- **relates_to:** map-branch-truce-desktop-implementation-baseline, practice-keep-failed-work-outstanding

## practice-follow-the-ui-design-brief-for-the-kde-native-window

- **kind:** practice
- **title:** Follow the UI design brief for the KDE-native window
- **path:** ui/practice-follow-the-ui-design-brief-for-the-kde-native-window.md
- **tags:** ui, design, kde, kirigami, qml
- **relates_to:** practice-design-desktop-apps-around-screens-and-components, practice-separate-save-from-mark-resolved, map-startup-style-colour-scheme-and-icon-theme

## practice-hold-a-git-call-to-test-racing-repository-changes

- **kind:** practice
- **title:** Hold a Git call to test racing repository changes
- **path:** git/practice-hold-a-git-call-to-test-racing-repository-changes.md
- **tags:** testing, git, concurrency, mergetool
- **relates_to:** map-recovery-journal-and-acknowledgment-boundary, map-mergetool-session-socket-and-real-git-parent, practice-fence-session-work-and-serialize-worktree-mutations

## practice-interpret-native-reference-comparisons-on-the-calibrated-grid

- **kind:** practice
- **title:** Interpret native reference comparisons on the calibrated grid
- **path:** ui/practice-interpret-native-reference-comparisons-on-the-calibrated-grid.md
- **tags:** testing, ui, capture, calibration, provenance
- **relates_to:** practice-wait-for-rendered-frames-before-capturing-native-scenes

## practice-keep-failed-work-outstanding

- **kind:** practice
- **title:** Keep failed work outstanding
- **path:** product/practice-keep-failed-work-outstanding.md
- **tags:** product, failures, saving, resolution
- **derived_from:** PRD.md, src/app/SessionModel.cpp, src/app/Application.cpp

## practice-keep-prd-focused-on-user-needs

- **kind:** practice
- **title:** Keep PRD.md focused on user needs
- **path:** product/practice-keep-prd-focused-on-user-needs.md
- **tags:** product, prd, documentation

## practice-keep-session-captures-immutable-across-revisions

- **kind:** practice
- **title:** Keep session captures immutable across revisions
- **path:** core/practice-keep-session-captures-immutable-across-revisions.md
- **tags:** session, snapshots, editing, source-identity
- **relates_to:** practice-keep-sources-identifiable-and-the-result-editable, practice-fence-session-work-and-serialize-worktree-mutations, map-conflict-admission-and-offline-startup

## practice-keep-sources-identifiable-and-the-result-editable

- **kind:** practice
- **title:** Keep sources identifiable and the result editable
- **path:** product/practice-keep-sources-identifiable-and-the-result-editable.md
- **tags:** product, comparison, editing
- **derived_from:** PRD.md, src/ui/ComparisonView.qml, src/ui/Main.qml, src/app/ResultEditorAdapter.cpp, docs/editor-integration.md

## practice-keep-techspecs-and-prds-independent

- **kind:** practice
- **title:** Keep TechSpecs and PRDs independent
- **path:** engineering/practice-keep-techspecs-and-prds-independent.md
- **tags:** documentation, techspec, prd
- **relates_to:** practice-keep-prd-focused-on-user-needs

## practice-keep-the-qmllint-target-at-zero-warnings

- **kind:** practice
- **title:** Keep the qmllint target at zero warnings
- **path:** ui/practice-keep-the-qmllint-target-at-zero-warnings.md
- **tags:** qml, lint, ui, ci
- **relates_to:** map-journey-acceptance-suite

## practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files

- **kind:** practice
- **title:** Keep ticket IDs and AI trailers out of commits and tracked files
- **path:** engineering/practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md
- **tags:** git, commits, ci, conventions, release

## practice-keep-unsupported-and-unfinished-work-visible

- **kind:** practice
- **title:** Keep unsupported and unfinished work visible
- **path:** product/practice-keep-unsupported-and-unfinished-work-visible.md
- **tags:** product, scope, progress, handoff
- **derived_from:** PRD.md, src/app/SessionModel.cpp, src/app/Application.cpp, src/ui/Main.qml, docs/session-state.md

## practice-make-conflict-choices-explicit-and-reversible

- **kind:** practice
- **title:** Make conflict choices explicit and reversible
- **path:** product/practice-make-conflict-choices-explicit-and-reversible.md
- **tags:** product, choices, editing, undo
- **derived_from:** PRD.md, src/core/EditingCommands.cpp, src/core/EditingCommands.md, src/core/SessionState.cpp, docs/recovery-journal.md

## practice-reconcile-uncertain-destination-publication

- **kind:** practice
- **title:** Reconcile uncertain destination publication
- **path:** recovery/practice-reconcile-uncertain-destination-publication.md
- **tags:** publication, storage, concurrency, git
- **relates_to:** practice-separate-save-from-mark-resolved, practice-fence-session-work-and-serialize-worktree-mutations, practice-keep-failed-work-outstanding

## practice-recover-unfinished-work-without-changing-its-status

- **kind:** practice
- **title:** Recover unfinished work without changing its status
- **path:** product/practice-recover-unfinished-work-without-changing-its-status.md
- **tags:** product, recovery, editing, progress
- **derived_from:** PRD.md, docs/recovery-journal.md, src/recovery/RecoveryInventory.cpp, src/publication/ResolutionPublisher.cpp

## practice-separate-save-from-mark-resolved

- **kind:** practice
- **title:** Separate Save from Mark resolved
- **path:** product/practice-separate-save-from-mark-resolved.md
- **tags:** product, saving, resolution, review
- **derived_from:** PRD.md, src/publication/ResolutionPublisher.cpp

## practice-treat-mergetool-file-state-as-live-evidence

- **kind:** practice
- **title:** Treat mergetool file state as live evidence
- **path:** git/practice-treat-mergetool-file-state-as-live-evidence.md
- **tags:** mergetool, publication, git, failures
- **relates_to:** practice-reconcile-uncertain-destination-publication, practice-recover-unfinished-work-without-changing-its-status, practice-keep-failed-work-outstanding, map-mergetool-session-socket-and-real-git-parent

## practice-wait-for-layout-before-clicking-offscreen-dialog-items

- **kind:** practice
- **title:** Wait for layout before clicking offscreen dialog items
- **path:** ui/practice-wait-for-layout-before-clicking-offscreen-dialog-items.md
- **tags:** testing, qml, ui, offscreen
- **relates_to:** practice-keep-unsupported-and-unfinished-work-visible, map-journey-acceptance-suite, practice-design-desktop-apps-around-screens-and-components

## practice-wait-for-rendered-frames-before-capturing-native-scenes

- **kind:** practice
- **title:** Wait for rendered frames before capturing native scenes
- **path:** ui/practice-wait-for-rendered-frames-before-capturing-native-scenes.md
- **tags:** testing, qml, ui, capture, readiness
- **relates_to:** practice-wait-for-layout-before-clicking-offscreen-dialog-items, map-startup-style-colour-scheme-and-icon-theme
