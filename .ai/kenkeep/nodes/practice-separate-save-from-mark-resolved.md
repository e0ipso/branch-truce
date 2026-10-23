---
type: practice
title: Separate Save from Mark resolved
description: >-
  Save keeps app-only drafts; Mark resolved writes the reviewed result and resolves the file in Git.
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
Save checkpoints incomplete work inside Branch Truce only. It changes neither the repository file nor the Git index, including when launched as Git's merge tool. Say that the draft is saved in Branch Truce and the repository file is unchanged. Keep automatic recovery, explicit Save checkpoints, repository writes and Git resolution distinct.

Mark resolved is the explicit review declaration. It saves the latest app draft, writes the reviewed result to the repository, and resolves it in Git without a prior Save. There is no separate review-only action. A failed write or Git resolution can leave reviewed work unresolved; show the actual outcome. Do not substitute an app progress indicator for verified Git resolution.

If source choices remain undecided, ask the user to confirm the current result is intentional before publishing it. Cancelling leaves text and choices unchanged. A later edit or undo invalidates review of the new draft. If an earlier result remains staged, show "Earlier result staged; current draft not applied" and block a successful finish until the new result is applied or deliberately discarded. Explicit leave after a warning remains possible. Never automatically recreate Git conflicts merely because the user edits.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
