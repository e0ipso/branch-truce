# Passage correspondence and revisioned anchors

`CorrespondenceAlgorithm` separates comparison from session bookkeeping. The
default `ExactLineCorrespondence` uses exact UTF-8 lines, including their newline
bytes, without whitespace folding. Hirschberg's divide-and-conquer alignment
uses length rows to find the first and last optimal crossing at each split.
These trace the two boundary paths of the optimal-alignment graph, including
equally good skips at equal lines. A line is equal only when both paths match
it to the same result line, so every optimal alignment agrees. Other matches
make the enclosing gap ambiguous. Gaps without possible matches describe
changed, source-only or result-only text.
The ordered passages partition both buffers and retain the original source
ranges for navigation, including uncertain regions and unavailable-versus-empty
sources.

This choice avoids a leftmost/rightmost tie-break that silently redirects a
choice after one of several equal passages disappears. Verified equal passages
support character-boundary byte ranges. The policy can reject useful choices,
but it does not invent certainty. Identical buffers take a direct path, so
repeated text with identical positions remains individually addressable.

A changed passage maps its outer boundaries. Binding an anchor also maps each
line start inside a changed passage by index, so a single line inside a
multi-line conflict has its own target. With N source lines against M result
lines, source line k pairs with result line k up to min(N, M). When N > M, the
surplus source lines map to an empty range at the end of the result block.
Accepting one inserts it there, after anything accepted there before, so
accepting them in source order keeps that order. A choice moves a competing
anchor on exactly the replaced range onto the new text, but another line from
the chosen side keeps its own insertion point after it. When M > N, the surplus
result lines stay with the last source line, because the passage end still maps
to the block end. The whole passage therefore still covers the whole block. A
source-only passage is the N:0 case. A changed passage holds no line equal to
one across from it, since that pair would lengthen the optimal alignment, so the
pairing never chooses between occurrences. Ambiguous passages, including
repeated identical lines, stay unmapped. Empty insertion anchors, mid-line
offsets and whole-buffer recomputation keep the edge-only rule. Selecting a
smaller part of a changed line needs manual editing rather than a substring
search.

One structural fact narrows ambiguity at launch. Git copies a side's
conflicting lines into that side's region only, so when the launch result's
marker lines form complete blocks with none left over, `settle` aligns each
side's source again with the result minus every block's marker lines, base
text and other side region. A line that the other region or the base also
holds then has one copy to match. An ambiguous anchor is mapped where every
optimal alignment of that text agrees and its range lies inside one stretch of
kept text. An empty anchor at the edge of removed text stays ambiguous, and so
does everything when a marker line is left over. Settling happens once, before
the first revision, so undo, redo and recovery replay start from the same
anchors.

The algorithm uses O(source lines + result lines) memory for different
buffers, including line, match and passage records. It runs linear-space Myers
first. D is the changed-line count: the source and result lines an alignment
skips, which is the same for every optimal alignment. A forward and a backward
search meet in the middle to find D. Then each splits the grid at the middle
skip of the first or last optimal path and recurses on both halves, so the
work is about the line count times D and no source-lines by result-lines matrix
exists. The first path is the lowest-left optimal path, and the last path
mirrors it. They are the paths the banded Hirschberg traces:
- After every h skips the first path is on the lowest diagonal any optimal path
  uses, and the last path is on the highest.
- Between the diagonals after h - 1 and h skips, the first path skips a source
  line at the first row that allows one and a result line at the last row. The
  last path skips a result line at the first row and a source line at the last.
- The extreme paths are searched on the whole texts, not only between the
  shared edges, because optimal paths can slide into the shared lines. D itself
  is the same for the whole texts and for their changed lines.

Myers is capped at D <= `kMaxAlignmentCells / (N' + M')`, where N' and M' are
the line counts between the shared start and end lines. It also stops after
2 * `kMaxAlignmentCells` work units, one for each diagonal step and each line
comparison, so long repetitive shared edges cannot run it for long. A pair over
either limit falls back to the banded Hirschberg. The lines both texts share at
their start and then at their end are a common subsequence, so their count `s`
is a lower bound for the LCS length. An optimal alignment skips at most
`source lines - s` source lines and `result lines - s` result lines, and only a
skip moves it off its diagonal, so every optimal path stays inside that band of
diagonals. The length rows visit only the band, which changes no boundary
crossing. Each recursion level then costs at most source lines times band width,
and the fallback runs when that cost is at most `kMaxAlignmentCells`. Each
split releases its length rows before recursing. Binding keeps one
line-start index per text and finds a paired line by binary search. Lines are
interned, so long-line bytes are compared during interning rather than at each
length calculation.

`CorrespondenceAlgorithm` states the contract every implementation keeps: valid
UTF-8 input inside the text envelope, a complete ordered partition, `Ambiguous`
where equally good alignments disagree, no state between calls, and
`std::bad_alloc` as the only failure. Choice sections and their decision IDs
come from the passages an algorithm returns, so each implementation names
itself with `identity()`. The recovery journal records that name with the
launch capture, and replay refuses a journal another algorithm wrote rather
than matching its decisions to different sections. `CorrespondenceMap` checks every text it
aligns against that input contract before the algorithm sees it, and
`FileSession` takes the algorithm it uses as an argument, with
`ExactLineCorrespondence` as the default. tests/core/CorrespondenceTests.cpp runs
the contract checks against the default and a minimal implementation.

The limits in ResourceLimits.h bound the input: no line over `kMaxLineBytes`
and no text over `kMaxRecoveryPayloadBytes`. A file whose result or captured
sources hold more than `kLargeFileLines` lines opens only after the user
confirms it, because alignment time can grow with the product of line counts.
Before allocating tokens or line records, `ExactLineCorrespondence` counts both
texts and their shared edges. `cost()` is 0 for identical texts and
`SIZE_MAX` when either text has more than `kMaxIndexedLines` lines. Otherwise it
is `min(old alignmentCost, (N' + M') * max(|N' - M'|, 1))`: the smaller of the
banded Hirschberg cost and a lower bound on Myers' work. It is an admission
estimate that takes time linear in the texts. It is never above the old band
cost, and `compare` limits its own work as described above. More than
`kMaxIndexedLines` lines in either text, or a pair that neither Myers nor the
fallback can align within its limit, yields one `Ambiguous` passage. Identical
texts need no workspace. A map charges each source's `cost()` to one cell
budget shared across its sources; a whole-buffer recomputation also charges its
old-to-new comparison to it.

The charge is an estimate, so the work ceiling is per compare, not per budget.
Myers stops within its budget of 2 * `kMaxAlignmentCells` work units. When
Myers fails, the fallback runs if the version 3 band cost estimate,
`alignmentCost`, is at most `kMaxAlignmentCells`. That estimate counts the
cells of one boundary pass per recursion level, not all the cell updates. The
two boundary passes, for the first and the last path, can update up to about
four times as many cells. Without a band, each pass updates about
2 * N * M cells over all its levels. This is the same as in version 3. So one
compare can do up to about 6 * `kMaxAlignmentCells` units of work. No combined
limit is enforced, because a lower limit would make some pairs that version 3
aligns into one `Ambiguous` passage. A map with three sources, or a
recomputation, can run that many compares.

The algorithm identity is `exact-line/4`. Version 3 used the banded
Hirschberg alone, so a pair over its cost was one `Ambiguous` passage. Version 4
adds Myers, so some of those pairs now align. Version 2 returned one
`Ambiguous` passage for every pair over the product limit. Journals written by
version 3 or earlier are refused rather than replayed with different passages.

Metadata has independent limits of 65536 entries and a conservative 16 MiB byte
estimate. Choice construction preflights each differing passage's line picks
and sections, reserving four entries per pick or section and 512 bytes plus
capture identity lengths per entry for choices, indexes, decision identifiers
and anchors. Non-conflicting targets preflight the same estimates against a
separate budget before generating IDs. Anchor binding separately charges its actual identity lengths
before copying them. Initial and replacement decision maps are bounded by
entry count and aggregate identifier bytes before copying; initialization
reserves the generated IDs before copying the initial map. Line indexes and
supported edited results are capped at 65536 lines, independently of the text
byte envelope. The 4000-line confirmation cannot waive these limits.

If comparison metadata cannot be admitted, the file opens as `OutsideApp`
with `MetadataTooLarge`. The current launch text, decisions and immutable raw
and converted captures remain available through the read APIs, with support
facts and the diagnostic for inspection and outside-app handoff. App editing,
choices, undo/redo, discards and Mark resolved stay blocked. The file stays
outstanding; no choices are truncated and no anchors are guessed. A rejected
or deferred large-file launch has an empty marker scan and no launch frames.
Confirming a large file retries full admission and either opens it or retains
this explicit fallback. Allocation failures open only that file as unreadable.
Measurements of the representative 4000-line size are in
tests/fixtures/performance/evidence.md.

Commands validate the complete ordered edit set and stream its final fragments
through the shared text/index policy before copying the draft. A shared
`kMaxAlignmentCells` work allowance charges each edit's text bytes and anchor
visits, request metadata and subsequent marker-cleanup edits. Oversized
commands return `TextTooLarge` without publishing a revision. Decision admission
is conservative and counts both current and submitted entries, including
repeated IDs. `edited` and `rebuilt` consume an unpublished map; callers copy
an immutable map explicitly, then move the candidate through its edits. Source
alignment runs only for the final candidate. Marker cleanup uses only original
result-owned marker lines that survive those edits, so inserted source text
cannot acquire marker ownership from provisional passages. Final alignment or
allocation failure leaves history and publication unchanged.

`SourceAnchor` carries the caller's stable contribution ID, immutable source
side/range and insertion affinity. `ResultAnchor` adds captured session, path and
source-object identities, the exact transaction revision, mapping safety,
manual-overlap state and a hand-edit flag. Contribution IDs cannot be rebound
to different source ranges. `FileSession::applyCommand` checks every target
against the current registered anchor and revision before changing either text
or decisions. It changes only those result ranges, leaving neighboring edits and
other decisions intact.
An overlapping manual change requires confirmation for that exact preview.
An ambiguous or invalidated anchor remains unavailable even with confirmation.

`editResult` takes an exact half-open UTF-8 byte edit. Insertions at either edge
of a nonempty anchor stay outside it; an empty anchor explicitly chooses before
or after. Interior edits transform the range and set both flags. Deleting a
complete target or crossing its boundary invalidates it. Automated replacements
retain their selected anchor, including a resulting empty range. A choice that
rebuilds a conflict block from the text its sections keep, in a new order, uses
`rebuilt`: an anchor inside kept text moves with it, an anchor on exactly the
dropped text is left empty where that text was, and the block's other anchors
are invalidated.

The two flags answer different questions. Manual overlap means a choice would
replace work other than the source, so replacing the range needs confirmation.
Binding sets it where the launch text already differs. The hand-edit flag
records editor changes only. A manual edit sets it on every anchor whose range
contains the edit or that the edit crosses, and `replaceDraft` sets it wherever
its comparison changed a range. A choice clears it on each anchor whose whole
range it replaces. That includes an invalidated or ambiguous anchor whose source
range lies inside the chosen one, since it has no result range left to compare.
It also includes such an anchor whose source, aligned against the text before
the choice, lies inside the replaced range. A competing line loses its range
that way when the user deletes the result line it pairs with.
A whole-file command clears it on all. A choice that only clears flags, with
text and decisions unchanged, still records a revision, so undo and recovery
replay bring the flags back.

Immediate and deferred edits check the total bytes, joined line lengths and
result line count before mutating text or anchors. An edit can leave the
alignment for later. `edited` with
`Realignment::Deferred` moves the anchors and hand edits at once, as above, but
does not compare the sources with the new text: the passages before and after
the edited lines keep their alignment, shifted past the edit, and the edited
lines with every passage they touch become one `Ambiguous` passage, so the
partition stays complete and claims no match it has not verified.
`realigned()` then builds the map an immediate edit would have built. It is a
const member of an immutable map, so a worker thread may run it while the
session goes on editing; `FileSession::installAlignment` keeps the result only
for the revision it was made for. Undo of a pending revision restores its
provisional passages, and Mark resolved aligns a pending revision before it
counts markers.

The whole-buffer `replaceDraft` API has no editor edit location. Its
old-to-new comparison transforms only ranges with unambiguous correspondence.
It never assumes a common prefix identifies the edited occurrence. Uncertain
anchors remain uncertain through later recomputation, even when similar text
reappears. Fresh source-to-result passages remain available for navigation.

Recovery replays a recorded revision with `replayDraft`: the same comparison
against the recorded text, then the recorded hand-edit flags and ranges, under
the recorded revision id. One record can stand for a burst of edits that were
joined into one undo entry (`EditOptions::join`).

Mapping values belong to revision snapshots. Undo, redo and checkpoint discard
restore text, decisions and mapping together while assigning the normal fresh
transaction revision. Previously issued previews remain stale. Source captures
stay immutable. These APIs perform no persistence, Qt or filesystem work.
[`EditingCommands`](EditingCommands.md) builds scoped choices on these mappings.
SessionModel owns recovery acknowledgment, and the Qt controller translates
exact editor changes without introducing a second undo history.
