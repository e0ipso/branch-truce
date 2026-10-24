---
type: practice
title: Bound each converted representation independently
description: >-
  Reject cleaned canonical objects over 4194304 bytes before reading; give
  each Git phase its own 4194304-byte budget.
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
[GitAdapter.cpp](../../../src/git/GitAdapter.cpp) gives each Git phase its own 4194304-byte output budget and resets the counter when that phase starts. Canonical blob and checkout reads are separate phases. Each phase counts stdout and stderr against the same budget. Preflight output does not reduce a later read.

An `OutputLimit` result leaves an incomplete captured prefix that cannot serve as complete content. The exact-boundary case in [GitAdapterTests.cpp](../../../tests/git/GitAdapterTests.cpp) covers canonical and checkout reads after preflight.

After clean conversion, `CanonicalizeText` checks the cleaned object's size with `git cat-file -s` before reading its blob. A size over 4194304 bytes returns `Outcome::Rejected` with the canonical byte count in the diagnostic and no canonical bytes. UTF-16LE input can expand beyond the limit during conversion, so the cleaned size needs its own check. [GitAdapterTests.cpp](../../../tests/git/GitAdapterTests.cpp) covers this expansion rejection. The independent read budgets still bound each representation, and [TextConversion.cpp](../../../src/git/TextConversion.cpp) separately verifies byte and canonical-object round trips.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
