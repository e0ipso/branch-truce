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
Present all currently conflicted files together in one session with a useful file list and visible progress. Prominently identify unsupported renamed, deleted, and image conflicts as requiring handling outside the app, both beside the file and in the session summary. Never count them as resolved because the app cannot handle them. Keep their notices visible when supported text work is complete.

Before handoff, show saved results, outstanding decisions, unsaved changes, review still needed, failures, and outside-app work. Offer a route to every outstanding file. Warn about unfinished work before any ordinary finish action, and let the user deliberately leave to handle it elsewhere without claiming success.

Keep navigation through undecided changes distinct from navigation through all differences. Let the user reach the next unfinished file and revisit saved or reviewed files. Preserve the session and the user's place when the file list closes and reopens. Do not treat completing every visible decision as proof that the content is correct.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
