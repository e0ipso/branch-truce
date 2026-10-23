---
type: practice
title: Keep failed work outstanding
description: >-
  Keep failed saves and resolutions visible, retain edited results, and claim
  resolution only after both steps succeed.
tags:
  - product
  - failures
  - saving
  - resolution
kk_schema_version: 3
kk_id: practice-keep-failed-work-outstanding
kk_derived_from:
  - PRD.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
When saving fails, identify the file, keep its edited result available, leave its unsaved warning visible, and offer retry or a safe way to remain in the session. Do not let a successful save of one file conceal another file's failure.

During Mark resolved, failure to save the app draft leaves the repository unchanged. If the repository write fails, retain any saved app draft and keep the file unresolved. If the repository write succeeds but Git resolution fails, show "written, unresolved" and offer retry. Claim current-result resolution only after repository writing and Git verification both succeed; an app draft checkpoint is a separate outcome.

If a file cannot be read or the session cannot show expected work, explain the problem and keep it outstanding. Do not treat a blank view as evidence of an empty file, no conflicts, or completion. Never claim saved, reviewed, or resolved work when that outcome does not hold.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
