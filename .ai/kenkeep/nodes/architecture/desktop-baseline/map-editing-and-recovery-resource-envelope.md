---
type: map
title: Editing and recovery resource envelope
description: >-
  Text, metadata, history, destination and recovery limits preserve unfinished
  work on refusal.
tags:
  - architecture
  - desktop
  - platform
  - cpp
kk_schema_version: 3
kk_id: map-editing-and-recovery-resource-envelope
kk_derived_from:
  - TECH_SPEC.md
  - src/ui/ComparisonView.qml
  - src/ui/Main.qml
  - src/app/Application.cpp
  - docs/editor-integration.md
  - docs/build.md
  - tests/fixtures/performance/evidence.md
  - src/core/ResourceLimits.h
  - docs/resource-limits.md
  - docs/mergetool-launcher.md
  - tests/app/ComparisonViewTests.cpp
  - tools/verify-install-prefix.sh
  - 'dcc586bc-c490-4186-893e-e9ac032ec77c:map:3'
kk_relates_to:
  - practice-design-desktop-apps-around-screens-and-components
  - map-branch-truce-first-release-scope
  - practice-treat-mergetool-file-state-as-live-evidence
  - map-mergetool-session-socket-and-real-git-parent
  - map-per-base-debian-artifacts-and-container-inspection
  - practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins
kk_depends_on: []
kk_confidence: high
---
Representative editing and recovery measurements are in [tests/fixtures/performance/evidence.md](../../../../../tests/fixtures/performance/evidence.md). The run is an offscreen software Release binary. [ResourceLimits.h](../../../../../src/core/ResourceLimits.h) enforces that envelope: 262144-byte lines, 4194304-byte Git reads, and 8388608-byte recovery payloads. See [docs/resource-limits.md](../../../../../docs/resource-limits.md). Oversized work fails explicitly and the previous buffer stays. Each Git phase starts a fresh 4194304-byte budget. The 4000-line measurement is a confirmation threshold, not a limit: a captured file whose result or a source has more lines opens as `Support::LargeFile`, and its placeholder asks before `FileSession::loadLargeFile` builds the correspondence. The journal records the load as `load-large`, which replay applies but does not count as user work. Edits past 4000 lines are accepted. Qt reports Backspace and Delete with a control character in `text()`, so `ResultEditorAdapter::eventFilter` excludes those keys from its insertion prediction. Both keys shorten a 262144-byte line or an 8388608-byte buffer, and an insertion past either limit is still rejected. [ComparisonViewTests.cpp](../../../../../tests/app/ComparisonViewTests.cpp) covers both limits. Changing the locked stack requires an explicit technical-spec decision.

The engine admits at most 65536 indexed lines and bounds metadata at 65536 entries / 16 MiB. History prunes the oldest undo prefix to 1024 entries / 256 MiB while protecting launch, current and publication-held revisions. Destination reads compare launch metadata before allocation, stop at 16908291 raw bytes and verify growth and identity afterward; deferred large files keep that same guarded baseline. Recovery frame, journal, payload, expanded-state and inventory admission are independent, as documented in `docs/recovery-journal.md`.

<!-- kk:related:start -->
# Related

- Related: [practice-design-desktop-apps-around-screens-and-components](../practice-design-desktop-apps-around-screens-and-components.md)
- Related: [map-branch-truce-first-release-scope](../../product/map-branch-truce-first-release-scope.md)
- Related: [practice-treat-mergetool-file-state-as-live-evidence](../../git/practice-treat-mergetool-file-state-as-live-evidence.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-launcher-decisions-and-reloads](../../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-mergetool-socket-lookup-and-access](../../git/mergetool/map-mergetool-socket-lookup-and-access.md)
- Related: [map-mergetool-session-socket-and-real-git-parent → map-real-git-parent-compatibility-and-fixture-isolation](../../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-developer-debian-artifact-builds-and-inspection](../../packaging/artifact-paths/map-developer-debian-artifact-builds-and-inspection.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-native-release-package-artifact-flow](../../packaging/artifact-paths/map-native-release-package-artifact-flow.md)
- Related: [map-per-base-debian-artifacts-and-container-inspection → map-runtime-qml-and-plugin-package-dependencies](../../packaging/artifact-paths/map-runtime-qml-and-plugin-package-dependencies.md)
- Related: [practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins](../../packaging/practice-compile-qml-to-bytecode-to-avoid-private-qt-abi-pins.md)
<!-- kk:related:end -->

<!-- kk:citations:start -->
# Citations

[1] [TECH_SPEC.md](../../../../../TECH_SPEC.md)
[2] [src/ui/ComparisonView.qml](../../../../../src/ui/ComparisonView.qml)
[3] [src/ui/Main.qml](../../../../../src/ui/Main.qml)
[4] [src/app/Application.cpp](../../../../../src/app/Application.cpp)
[5] [docs/editor-integration.md](../../../../../docs/editor-integration.md)
[6] [docs/build.md](../../../../../docs/build.md)
[7] [tests/fixtures/performance/evidence.md](../../../../../tests/fixtures/performance/evidence.md)
[8] [src/core/ResourceLimits.h](../../../../../src/core/ResourceLimits.h)
[9] [docs/resource-limits.md](../../../../../docs/resource-limits.md)
[10] [docs/mergetool-launcher.md](../../../../../docs/mergetool-launcher.md)
[11] [tests/app/ComparisonViewTests.cpp](../../../../../tests/app/ComparisonViewTests.cpp)
[12] [tools/verify-install-prefix.sh](../../../../../tools/verify-install-prefix.sh)
[13] dcc586bc-c490-4186-893e-e9ac032ec77c:map:3
<!-- kk:citations:end -->
