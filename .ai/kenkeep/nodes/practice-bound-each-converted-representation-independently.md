---
type: practice
title: Bound each converted representation independently
description: >-
  Reject cleaned canonical objects over 8 MiB before reading; give canonical
  blob and checkout reads separate 8 MiB budgets.
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

After clean conversion, `CanonicalizeText` checks the cleaned object's size with `git cat-file -s` before reading its blob. A size over 8 MiB returns `Outcome::Rejected` with the canonical byte count in the diagnostic and no canonical bytes. UTF-16LE input can expand beyond the limit during conversion, so the cleaned size needs its own check. [GitAdapterTests.cpp](../../../tests/git/GitAdapterTests.cpp) covers this expansion rejection. The independent read budgets still bound each representation, and [TextConversion.cpp](../../../src/git/TextConversion.cpp) separately verifies byte and canonical-object round trips.

<!-- kk:related:start -->
# Related

- Related: [practice-keep-failed-work-outstanding](practice-keep-failed-work-outstanding.md)
<!-- kk:related:end -->
