---
type: practice
title: Bound each converted representation independently
description: >-
  Canonical blob and checkout reads each get 8 MiB; validation does not
  guarantee that expanding encodings fit every stage.
tags:
  - git
  - conversion
  - limits
  - failures
kk_schema_version: 3
kk_id: practice-bound-each-converted-representation-independently
kk_derived_from: []
kk_relates_to:
  - practice-keep-failed-work-outstanding
kk_depends_on: []
kk_confidence: high
---
[GitAdapter.cpp](../../../src/git/GitAdapter.cpp) gives canonical blob and checkout reads separate 8 MiB output budgets and resets the counter for each phase after preflight. Each phase counts stdout and stderr against the same budget. Keep these budgets separate so preflight or the first representation cannot consume the next representation's allowance.

An `OutputLimit` result leaves an incomplete captured prefix that cannot serve as complete content. The exact-boundary case in [GitAdapterTests.cpp](../../../tests/git/GitAdapterTests.cpp) covers canonical and checkout reads after preflight.

Separate budgets and successful validation do not guarantee that every accepted input fits every later representation. UTF-16LE input can pass validation and still exceed the output budget during clean conversion, returning `OutputLimit`. [TextFormat.cpp](../../../src/storage/TextFormat.cpp) handles decoding and serialization. [TextConversion.cpp](../../../src/git/TextConversion.cpp) separately verifies byte and canonical-object round trips and propagates output-limit failures.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
