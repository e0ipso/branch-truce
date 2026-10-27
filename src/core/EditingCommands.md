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
preserves every result byte, unless it completes a marker section as described
below. ResolveUsingSide accepts the selected source and ignores competitors in
that section. A line resolution requires exact matching competing line ranges.
Ambiguous or invalidated mappings require direct editing.

A marker section is a complete conflict-marker block whose marker lines and
base text match a block in the launch result, together with the choice
sections it encloses. Either its two side regions hold exactly the mapped
scopes of one section, or Git wrote several sections into one block around
lines both sides share: then each side region holds one mapped scope of every
section, in the same order and with the same lines around them on both sides.
A section with a deletion, or shared lines that differ, leave the block
untied. `FileSession::markerSections` lists them. When a command
decides the last open contribution of a marker section and each of its
sections has an accepted contribution, the same history entry deletes the
block's marker lines, its base text and the text of its ignored contributions.
Of the shared lines Git copied into both sides, one copy goes. One undo brings
back the markers, text and decisions together. Accept alone keeps the markers,
and so does ignoring both sides of any section. Deleting text cannot reorder
it, so the left text kept must come before the right text kept: a block whose
earlier section keeps right text and whose later section keeps left text
keeps its markers. A hand edit to a marker line or to the base text orphans
the block.
Ignored text that holds a hand edit or pre-launch work keeps the whole block.
In all these cases the markers stay for direct editing and keep blocking Mark
resolved. A deletion side fills only an empty region. The preview says when a choice removes the markers.

A source deletion uses the mapped competing section as its replacement range
and validates both anchors. It cannot infer a deletion range from neighboring
text. In a marker section the deletion's insertion point sits past the end
marker, so only the competing anchor is checked: `markerSections` already
requires it to be exactly that side's region inside the block. Accept never silently replaces an accepted competing contribution.
A section choice unmaps the line anchors inside it, so an accepted line of
another section is located by its side's section anchor instead. The accept
is refused when neither anchor is mapped or the located range overlaps.
Undo the intervening actions before changing an earlier accepted decision.

Overlapping manual work requires an exact before/replacement preview and
confirmation. `applyChoice` regenerates and compares that preview at its issued
revision, so confirmation cannot authorize stale or modified content. Cancel
changes neither text nor decisions.

`EditingCommand` is the shared value interface for mapped edits and explicit
decision updates. `FileSession::applyCommand` validates all current registered
targets, UTF-8 boundaries and nonoverlapping edits before publishing one history
entry. It supports planners that need several ranges or ordered decisions.
Its explicit `WholeFile` scope requires one full-result edit and confirmation,
so whole-file choices do not depend on an uncertain passage mapping.

`previewWholeFile` offers either captured source, including an empty source,
without requiring an earlier version. It shows the complete current and proposed
result, every differing passage including pre-existing and manual work, and the
source contributions with their current and proposed decisions. The chosen
source's recorded contributions become accepted; the other source's become
ignored. Previous combination orders are cleared. Earlier recovered drafts keep
their exact decision vocabulary, and decisions without a source scope remain
unchanged. The preview explains the text and contributions given up.

Whole-file confirmation regenerates and compares the entire preview, including
session, file, source identity and current revision. Cancellation changes no text,
decisions or history. Stale or modified previews require renewed inspection.
One shared `EditingCommand` replaces the full result and decisions atomically.
The model persists it through the existing `replace-draft` recovery transaction
before acknowledgment. Undo and redo restore complete text and decisions through
the same boundary, including after journal replay. A failed commit retains the
complete pending candidate for retry. Replacement leaves Save, review and Git
resolution as separate actions.

`previewNonConflicting` offers Left, Right and Both batches for the selected
file. It compares each captured side against the available common earlier
version. Changed ranges that overlap the other side's changes or ambiguous
alignment stay outstanding. Insertions touching another changed range also
stay outstanding, since their order can matter. Identical changes are eligible
from either side and appear once in a combined batch. Without an earlier
snapshot, the batch is unavailable.

Each eligible contribution has registered source and earlier-version anchors
that follow edits and history. A batch replaces only mapped, unchanged earlier
text, or accepts an exactly mapped contribution already present in the result.
Existing decisions, overlapping manual edits, unsafe mappings and overlapping
accepted contributions are skipped. A combined batch also rejects independent
changes whose result ranges collide. Edits outside or between its ranges remain
intact. Only selected source decisions become accepted; competing decisions keep
their exact state. Older drafts keep their existing decision vocabulary.

The batch preview names the file, side scope, eligible and skipped counts, source
line ranges, and exact current/proposed text for each change. Applying requires
explicit confirmation and regenerates the entire preview at its issued revision.
Cancellation, stale previews and altered previews leave text and decisions alone.
The complete batch is one `EditingCommand` and one recovery transaction, so undo
and redo restore its text and decisions in the shared action order. Failed
recovery retains the whole batch candidate for retry before acknowledgment.

`CombineLeftFirst` and `CombineRightFirst` preview both immutable source scopes
in their stated order. A line pairs with exactly one mapped competing line in
the same section; a section pairs with its competing section. A unique identical
result range takes priority over adjacent ranges. Without an exact match, a
unique adjacent range is required. The two sides of a marker section sit apart,
across its separator line, and still pair with each other: the combination
fills the first side's region and empties the other before the markers go.
Unequal overlapping ranges, repeated passages
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
