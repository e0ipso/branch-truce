---
type: map
title: Conflict admission and offline startup
description: >-
  ConflictClassifier admits editable merge/rebase files; startup preserves
  excluded reasons and existing manual edits.
tags:
  - git
  - classification
  - conversion
  - startup
  - scope
kk_schema_version: 3
kk_id: map-conflict-admission-and-offline-startup
kk_derived_from: []
kk_relates_to:
  - map-repository-discovery-and-verified-source-identities
  - practice-keep-unsupported-and-unfinished-work-visible
  - practice-keep-sources-identifiable-and-the-result-editable
  - practice-bound-each-converted-representation-independently
kk_depends_on: []
kk_confidence: high
---
[ConflictClassifier](../../../src/git/ConflictClassifier.h) is the read-only admission boundary for a complete conflict. Discovery's repository `supported` flag establishes operation identity only. Before `Conflict::editable()` permits editing, the classifier requires unique regular-file stages, present stages 2 and 3 with an agreed executable mode, valid topology, verified sources, successful source conversions, and a readable, decodable launch result. Its [implementation](../../../src/git/ConflictClassifier.cpp) retains every excluded path with a specific outside-app reason, including invalid topology. It rechecks the operation, index, current result, and conversion configuration before returning.

[TextConversion.cpp](../../../src/git/TextConversion.cpp) rejects unsupported or contradictory `text`, `eol`, and `ident` attributes. Every available source must preserve its worktree bytes through clean/checkout and return to the captured canonical object identity. Conversions that change EOL, BOM, ident expansion, or canonical bytes remain excluded with reasons. These round-trip checks remain separate from the [per-representation output limits](practice-bound-each-converted-representation-independently.md).

[ApplicationController::openRepository](../../../src/app/ApplicationController.cpp) starts classification offline, directly from a selected local merge or rebase, without a separate online step. Startup takes each file's support and reason from the classifier. Excluded files stay outside the app with their reasons. Unsupported selections remain explicit.

Accepted files open with their captured working-file results, preserving manual UTF-8 edits instead of regenerating content from a source. [StartupRouteTests.cpp](../../../tests/app/StartupRouteTests.cpp) covers excluded attribute reasons alongside an accepted edited result.

<!-- kk:related:start -->
# Related

- Related: [map-repository-discovery-and-verified-source-identities](map-repository-discovery-and-verified-source-identities.md)
- Related: [practice-keep-unsupported-and-unfinished-work-visible](practice-keep-unsupported-and-unfinished-work-visible.md)
- Related: [practice-keep-sources-identifiable-and-the-result-editable](practice-keep-sources-identifiable-and-the-result-editable.md)
- Related: [practice-bound-each-converted-representation-independently](practice-bound-each-converted-representation-independently.md)
<!-- kk:related:end -->
