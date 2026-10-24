# Scoped choices and application history

`choiceTargets` groups differing passages between the two immutable captured
sides into sections. Each nonempty source line has a stable byte-range decision
ID. Section controls share those IDs with their line controls. An empty source
scope represents a deletion and has a section control, without a misleading
individual-line control. Equal source passages have no choice controls.
The catalogue stores a linear number of targets and decision references.

New repository loads initialize these decisions before the first recovery
record. Earlier recovery records retain their existing decisions and status;
scopes absent from those records are unavailable rather than silently adding
new judgments. Direct editing remains available for supported results.

`previewChoice` binds Accept, Ignore or ResolveUsingSide to the current revision
and registered source anchor. Accept changes only its mapped range and leaves
the competing decision undecided. Ignore changes only undecided decisions and
preserves every result byte. ResolveUsingSide accepts the selected source and
ignores competitors in that section. A line resolution requires exact matching
competing line ranges. Ambiguous or invalidated mappings require direct editing.

A source deletion uses the mapped competing section as its replacement range
and validates both anchors. It cannot infer a deletion range from neighboring
text. Accept never silently replaces an accepted competing contribution.
Undo the intervening actions before changing an earlier accepted decision.

Overlapping manual work requires an exact before/replacement preview and
confirmation. `applyChoice` regenerates and compares that preview at its issued
revision, so confirmation cannot authorize stale or modified content. Cancel
changes neither text nor decisions.

`EditingCommand` is the shared value interface for mapped edits and explicit
decision updates. `FileSession::applyCommand` validates all current registered
targets, UTF-8 boundaries and nonoverlapping edits before publishing one history
entry. It supports later planners that need several ranges or ordered decisions.
Bulk and whole-file workflows are separate work.

`CombineLeftFirst` and `CombineRightFirst` preview both immutable source scopes
in their stated order. A line pairs with exactly one mapped competing line in
the same section; a section pairs with its competing section. A unique identical
result range takes priority over adjacent ranges. Without an exact match, a
unique adjacent range is required. Unequal overlapping ranges, repeated passages
without unique correspondence, and replacements that would erase unselected
contributions require a larger safe scope or direct editing.

The preview captures the source order, exact current and proposed text, affected
result range and whether that scope includes manual or pre-existing work.
Concatenation preserves the selected bytes, including final-newline behavior;
it does not silently deduplicate or claim a correct merge. Every combination
requires confirmation. Cancelling changes nothing, and applying regenerates the
whole preview to reject stale or modified content. A new preview can reorder
decided scopes or replace later overlapping edits. It records both contributions
as accepted with explicit order 1 and 2, while preserving all other decisions.
Adjacent already-present contributions are replaced atomically rather than
appended a second time. Manual edits outside the displayed range remain intact.

Manual edits use exact UTF-8 byte ranges. The Qt adapter converts scalar-safe
UTF-16 document changes to those ranges and disables the document's native undo
stack. Toolbar and standard keyboard undo/redo use the same FileSession history
as choices. Text, decisions and correspondence restore together with fresh
revision identities; Save retains that history.

SessionModel executes each operation on a private candidate. Manual edits,
choices and generic mapped commands persist complete `replace-draft` recovery
transactions before acknowledgment. Undo and redo have their existing durable
actions. Journal replay reproduces the same text, decision and history order.
A failed recovery commit retains the candidate visibly pending, keeps the last
acknowledged state unchanged and blocks further commands until retry succeeds.
Restored, unsaved and uncertain publication states remain separate from choices.
