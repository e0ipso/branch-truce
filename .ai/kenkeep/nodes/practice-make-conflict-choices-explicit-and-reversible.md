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
  - src/core/EditingCommands.cpp
  - src/core/EditingCommands.md
  - src/core/SessionState.cpp
  - docs/recovery-journal.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Provide a labeled accept arrow and ignore X for each side's change. Accepting one contribution leaves the competing contribution undecided. Keep 'resolve using this side' distinct: it accepts that contribution and rejects its competitor for the selected region. Make its effect clear before use.

Treat Ignore as deciding against an undecided contribution without changing the current result, including manual edits. Leave the competing contribution undecided. Do not use Ignore to remove accepted text; require undoing acceptance first. Undoing Ignore makes that contribution undecided again.

Allow individual-line choices, whole-section choices, and direct result editing. Show each choice's scope and leave other choices available. Allow both contributions with explicit ordering. After manual edits, preview and confirm any overlapping replacement; cancel without changing text or decisions. If the combination or its target is ambiguous, require manual editing rather than guessing. Do not present omitted edits or duplicated text as a clean combination. Undo follows the history stack; it does not selectively remove an old contribution through newer edits.

Limit bulk non-conflict choices to the stated file and changes, leaving competing choices and unrelated manual edits alone. Explain whole-file replacement, including contributions and manual edits it gives up, and allow inspection, undo, and review before saving. Make undo and redo restore both text and undecided states for manual edits and choices at every supported scope.

Scoped Accept, Ignore and resolve using one side in [EditingCommands](../../../src/core/EditingCommands.cpp) share [SessionState](../../../src/core/SessionState.cpp) history with manual edits. Undo and redo restore text and decision state together. [SessionModel](../../../src/app/SessionModel.cpp) commits each completed command, undo and redo across the recovery boundary before acknowledgment. Earlier recovered drafts retain their existing decision vocabulary. When a scoped choice is absent or cannot be mapped safely, keep it unavailable and leave direct result editing available for otherwise editable files.

`CombineLeftFirst` and `CombineRightFirst` preview both immutable source scopes and require confirmation. `applyChoice` rebuilds that preview at the issued revision and rejects a stale or altered preview. The confirmed combination is one `EditingCommand`, one history entry, and one `replace-draft` recovery transaction.

Left, Right, and Both batches apply only to the selected file. `previewNonConflicting` stays unavailable when the common earlier snapshot is absent. Ambiguous alignment stays outstanding, and so does an insertion that touches another changed range. A batch leaves competing decisions unchanged and leaves unrelated manual edits in place. Confirmation applies the regenerated preview. Cancellation and a stale preview leave text and decisions unchanged.

`previewWholeFile` previews either captured source. `applyWholeFile` applies only the confirmed current revision. Recovered decisions that have no source scope stay unchanged.

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
[2] [src/core/EditingCommands.cpp](../../../src/core/EditingCommands.cpp)
[3] [src/core/EditingCommands.md](../../../src/core/EditingCommands.md)
[4] [src/core/SessionState.cpp](../../../src/core/SessionState.cpp)
[5] [docs/recovery-journal.md](../../../docs/recovery-journal.md)
<!-- kk:citations:end -->
