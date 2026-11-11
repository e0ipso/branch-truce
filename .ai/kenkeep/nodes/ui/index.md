# kenkeep Index: ui

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
_None._

## Conventions (how we build)
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) to learn about: In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles. #testing #qml #ui #offscreen
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) to learn about: docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision. #ui #design #kde #kirigami #qml
- Open [**Wait for rendered frames before capturing native scenes**](practice-wait-for-rendered-frames-before-capturing-native-scenes.md) to learn about: Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus. #testing #qml #ui #capture #readiness
- Open [**Interpret native reference comparisons on the calibrated grid**](practice-interpret-native-reference-comparisons-on-the-calibrated-grid.md) to learn about: Keep raster scale, comparison-cell units and capture provenance explicit when judging native screenshots against approved references. #testing #ui #capture #calibration #provenance
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) to learn about: src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites. #qml #lint #ui #ci

## Components (what exists)
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) to learn about: CTest acceptance groups drive Main offscreen over real Git merges and rebases with private XDG directories and an offline Git guard. #testing #acceptance #qml #offscreen #git
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) to learn about: main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it. #ui #startup #kde #icons #testing #preview
- Open [**Literal project text in native controls**](map-literal-project-text-in-native-controls.md) to learn about: File rows and tooltips keep native backing text empty so retained auto-text labels never receive repository markup. #ui #qml #paths #testing
- Open [**Result conflict-marker folding**](map-result-conflict-marker-folding.md) to learn about: TextBlocks folds recognized conflict content without changing result bytes, line numbers or application history. #comparison #qml #editing #markers
- Open [**Revision-bound source control presentation**](map-revision-bound-source-control-presentation.md) to learn about: Stable source-row models publish complete comparison snapshots and update command fields without recreating unaffected controls. #comparison #qml #editing #revisions

## By topic

### #qml
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Literal project text in native controls**](map-literal-project-text-in-native-controls.md) — File rows and tooltips keep native backing text empty so retained auto-text labels never receive repository markup.
- Open [**Wait for rendered frames before capturing native scenes**](practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
### #ui
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Literal project text in native controls**](map-literal-project-text-in-native-controls.md) — File rows and tooltips keep native backing text empty so retained auto-text labels never receive repository markup.
- Open [**Wait for rendered frames before capturing native scenes**](practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
### #testing
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
- Open [**Literal project text in native controls**](map-literal-project-text-in-native-controls.md) — File rows and tooltips keep native backing text empty so retained auto-text labels never receive repository markup.
- Open [**Wait for rendered frames before capturing native scenes**](practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
### #capture
- Open [**Wait for rendered frames before capturing native scenes**](practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
- Open [**Interpret native reference comparisons on the calibrated grid**](practice-interpret-native-reference-comparisons-on-the-calibrated-grid.md) — Keep raster scale, comparison-cell units and capture provenance explicit when judging native screenshots against approved references.
### #comparison
- Open [**Revision-bound source control presentation**](map-revision-bound-source-control-presentation.md) — Stable source-row models publish complete comparison snapshots and update command fields without recreating unaffected controls.
- Open [**Revision-bound passage correspondence**](../core/map-revision-bound-passage-correspondence.md) — Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear.
- Open [**Result conflict-marker folding**](map-result-conflict-marker-folding.md) — TextBlocks folds recognized conflict content without changing result bytes, line numbers or application history.
### #editing
- Open [**Recovery acknowledgment and retained input**](../recovery/journal/map-recovery-acknowledgment-and-retained-input.md) — One bounded serial worker computes and persists commands; failures retain exact pending transactions for retry.
- Open [**Recovery journal format and validation**](../recovery/journal/map-recovery-journal-format-and-validation.md) — Version 3 frames and complete domain checkpoints preserve retained history and reject invalid recovery.
- Open [**Recovery of copied launches and interrupted publication**](../recovery/journal/map-recovery-of-copied-launches-and-interrupted-publication.md) — Copied launch destinations and interrupted mutations require current evidence and durable observations.
### #kde
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
### #offscreen
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) — CTest acceptance groups drive Main offscreen over real Git merges and rebases with private XDG directories and an offline Git guard.
- Open [**Wait for layout before clicking offscreen dialog items**](practice-wait-for-layout-before-clicking-offscreen-dialog-items.md) — In offscreen UI tests, find popup delegates through the visual item tree and click only after their scene position settles.
### #acceptance
- Open [**Journey acceptance suite**](map-journey-acceptance-suite.md) — CTest acceptance groups drive Main offscreen over real Git merges and rebases with private XDG directories and an offline Git guard.
### #calibration
- Open [**Interpret native reference comparisons on the calibrated grid**](practice-interpret-native-reference-comparisons-on-the-calibrated-grid.md) — Keep raster scale, comparison-cell units and capture provenance explicit when judging native screenshots against approved references.
### #ci
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](../engineering/practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #design
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
### #git
- Open [**Mergetool launcher decisions and reloads**](../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](../git/mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #icons
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
### #kirigami
- Open [**Follow the UI design brief for the KDE-native window**](practice-follow-the-ui-design-brief-for-the-kde-native-window.md) — docs/ui-design.md sets layout, actions and colour roles; panes get the space and red marks only ranges that need a decision.
### #lint
- Open [**Keep the qmllint target at zero warnings**](practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
### #markers
- Open [**Conflict marker scan**](../core/map-conflict-marker-scan.md) — FileSession derives marker counts from complete blocks and stray lines while preserving marker-like source text.
- Open [**Conflict marker sections**](../core/map-conflict-marker-sections.md) — Marker sections tie intact Git conflict frames to choices and clean up fully decided blocks in the same undo entry.
- Open [**Result conflict-marker folding**](map-result-conflict-marker-folding.md) — TextBlocks folds recognized conflict content without changing result bytes, line numbers or application history.
### #paths
- Open [**Literal project text in native controls**](map-literal-project-text-in-native-controls.md) — File rows and tooltips keep native backing text empty so retained auto-text labels never receive repository markup.
### #preview
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
### #provenance
- Open [**Interpret native reference comparisons on the calibrated grid**](practice-interpret-native-reference-comparisons-on-the-calibrated-grid.md) — Keep raster scale, comparison-cell units and capture provenance explicit when judging native screenshots against approved references.
### #readiness
- Open [**Wait for rendered frames before capturing native scenes**](practice-wait-for-rendered-frames-before-capturing-native-scenes.md) — Capture native replay after publication, layout and rendered frames settle while preserving real controls and focus.
### #revisions
- Open [**Revision-bound passage correspondence**](../core/map-revision-bound-passage-correspondence.md) — Map immutable source ranges to one result revision; reject uncertain targets and keep alignment memory linear.
- Open [**Revision-bound source control presentation**](map-revision-bound-source-control-presentation.md) — Stable source-row models publish complete comparison snapshots and update command fields without recreating unaffected controls.
### #startup
- Open [**Startup style, colour scheme and icon theme**](map-startup-style-colour-scheme-and-icon-theme.md) — main.cpp picks org.kde.desktop or Fusion, names hicolor when no icon theme is set; kdeglobals sets colours; a preloaded probe tests it.
- Open [**Conflict admission and offline startup**](../git/map-conflict-admission-and-offline-startup.md) — ConflictClassifier admits editable merge/rebase files; startup preserves excluded reasons and existing manual edits.