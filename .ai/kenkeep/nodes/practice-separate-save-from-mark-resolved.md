---
type: practice
title: Separate Save from Mark resolved
description: >-
  Save preserves drafts; Mark resolved saves the reviewed result and resolves
  the file in the existing Git workflow.
tags:
  - product
  - saving
  - resolution
  - review
kk_schema_version: 3
kk_id: practice-separate-save-from-mark-resolved
kk_derived_from:
  - PRD.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Let Save preserve incomplete work without declaring the file reviewed or resolved. Keep saved unfinished work visibly unfinished, and distinguish unsaved, saved-but-unresolved, and resolved work.

After review, let 'Mark resolved' save the latest result and mark the file resolved in the user's existing Git workflow without requiring a separate Save. Do not substitute a change to the app's progress indicator for resolving the file in that workflow.

If source choices remain undecided, ask the user to confirm that the current result is their intentional, reviewed resolution before saving and marking it resolved. Cancelling leaves text and choices unchanged. Confirming accepts the reviewed result without requiring additional source selections. Require review again after a later edit and remove any misleading resolved status.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
