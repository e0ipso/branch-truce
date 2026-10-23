---
type: practice
title: Keep unsupported and unfinished work visible
description: >-
  Show all current files, prominent outside-app warnings, and an honest handoff
  that directs users to outstanding work.
tags:
  - product
  - scope
  - progress
  - handoff
kk_schema_version: 3
kk_id: practice-keep-unsupported-and-unfinished-work-visible
kk_derived_from:
  - PRD.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Present all currently conflicted files together in one session with a useful file list and visible progress. Prominently identify unsupported rename-specific, deletion, image, file-type, permission and conversion cases as outside-app work. A file is not unsupported merely because it was renamed earlier; resolving disputes about moves or destinations remains outside the first release. Never count unsupported work as resolved. Keep its notices visible when supported text work is complete.

Before handoff, distinguish saved app drafts, repository results, outstanding decisions, unsaved changes, review needed, failures and outside-app work. A newer draft than an earlier staged result blocks a successful finish and needs an explicit stale-staged warning. Offer a route to each outstanding file. The user may deliberately leave after a warning without a success claim.

Keep navigation through undecided changes distinct from navigation through all differences. Let the user reach the next unfinished file and revisit saved or reviewed files. Preserve the session and the user's place when the file list closes and reopens. Do not treat completing every visible decision as proof that the content is correct.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
