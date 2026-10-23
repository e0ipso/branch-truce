---
type: practice
title: Recover unfinished work without changing its status
description: >-
  Restore unsaved edits and outstanding decisions after closing or a crash while
  preserving honest work status.
tags:
  - product
  - recovery
  - editing
  - progress
kk_schema_version: 3
kk_id: practice-recover-unfinished-work-without-changing-its-status
kk_derived_from:
  - PRD.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Restore completed edits and outstanding decisions after closing the app or an application crash, including edits not manually saved. Every acknowledged editing transaction must be recoverable; visibly pending input or composition may need repeating. Do not acknowledge editing while recovery persistence is failing. This is not a machine-power-loss guarantee. Keep recovered drafts not manually saved visibly unsaved, and never treat recovery as review or Git resolution.

Keep the latest recoverable state separate from the last explicit Save checkpoint. A saved draft remains app-owned until Mark resolved publishes it. Deliberate discard must prevent the discarded state from becoming the current restored result.

Prevent switching files or closing the app from silently losing unsaved edits. Explain save, leave, and discard consequences, including which earlier saves remain. Keep opening or inspecting work from changing content or making choices for the user.

If recovery cannot restore expected work, explain what is missing and keep the affected file outstanding. Never claim recovery of work that is missing.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
