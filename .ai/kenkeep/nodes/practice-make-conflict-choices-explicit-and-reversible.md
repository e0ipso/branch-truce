---
type: practice
title: Make conflict choices explicit and reversible
description: >-
  Keep accept, ignore, combine, and broader choices distinct, inspectable, and
  reversible with their decision states.
tags:
  - product
  - choices
  - editing
  - undo
kk_schema_version: 3
kk_id: practice-make-conflict-choices-explicit-and-reversible
kk_derived_from:
  - PRD.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Provide a labeled accept arrow and ignore X for each side's change. Accepting one contribution leaves the competing contribution undecided. Keep 'resolve using this side' distinct: it accepts that contribution and rejects its competitor for the selected region. Make its effect clear before use.

Treat Ignore as deciding against an undecided contribution without changing the current result, including manual edits. Leave the competing contribution undecided. Do not use Ignore to remove accepted text; require undoing acceptance first. Undoing Ignore makes that contribution undecided again.

Allow individual-line choices, whole-section choices, and direct result editing. Show each choice's scope and leave other choices available. Allow both contributions with explicit ordering. After manual edits, preview and confirm any overlapping replacement; cancel without changing text or decisions. If the combination or its target is ambiguous, require manual editing rather than guessing. Do not present omitted edits or duplicated text as a clean combination. Undo follows the history stack; it does not selectively remove an old contribution through newer edits.

Limit bulk non-conflict choices to the stated file and changes, leaving competing choices and unrelated manual edits alone. Explain whole-file replacement, including contributions and manual edits it gives up, and allow inspection, undo, and review before saving. Make undo and redo restore both text and undecided states for manual edits and choices at every supported scope.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
<!-- kk:citations:end -->
