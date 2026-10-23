---
type: map
title: Branch Truce first-release scope
description: >-
  PRD.md defines a standalone, offline Linux app for text conflicts during
  merges and rebases.
tags:
  - product
  - scope
  - workflow
kk_schema_version: 3
kk_id: map-branch-truce-first-release-scope
kk_derived_from:
  - PRD.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
PRD.md defines Branch Truce as a standalone Linux app launched from the user's existing Git workflow, including use as the configured Git merge tool. Its purpose is to replace the conflict resolver the user retains an IDE for, let them understand competing contributions, compose the intended result, and return to ordinary work. The requirements mandate entirely offline use without an account, sign-in, subscription, or payment flow.

The first-release requirements cover text conflicts during merging and rebasing. Other operations must not be mislabeled as supported merge/rebase work or offered resolution. Detecting changes made in other apps and offering a comparison to reconcile them are outside the first release. Save preserves app-owned drafts; only Mark resolved publishes the reviewed result to the repository and Git workflow.

The first release requires mouse controls and basic result-editing shortcuts, including copying, pasting, undo, and redo. Full keyboard operation across files, comparisons, conflict choices, review, and saving is outside the first-release scope.

The defined session covers the work launched into it and makes no promise about conflicts that appear after continuation elsewhere. Code checks remain in the user's regular editor. Continuing the operation and committing remain in the user's usual workflow.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
