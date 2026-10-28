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

The algorithm uses O(source lines * result lines) time and O(source lines +
result lines) memory for different buffers, including line, match and passage
records. Each split releases its length rows before recursing. Binding keeps one
line-start index per text and finds a paired line by binary search. Lines are
interned, so long-line bytes are compared during interning rather than at each
length calculation. Initial correspondence failure marks only that file session
unreadable with a path-specific diagnostic, retaining its captured sources and
launch result. Other files can finish loading. Allocation or representation
failure during later edits still throws before a new session revision is
published. There is no enforced file-size or latency limit. Representative
measurements and a proposed envelope for large-file performance are in
tests/fixtures/performance/evidence.md. Product output, line and recovery
limits are unchanged.

`SourceAnchor` carries the caller's stable contribution ID, immutable source
side/range and insertion affinity. `ResultAnchor` adds captured session, path and
source-object identities, the exact transaction revision, mapping safety,
manual-overlap state and a hand-edit flag. Contribution IDs cannot be rebound
to different source ranges. `FileSession::replaceMapped` checks the current
registered anchor and revision before changing either text or the
contribution's decision. It changes only that result range, leaving
neighboring edits and other decisions intact.
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

The existing whole-buffer `replaceDraft` API has no editor edit location. Its
old-to-new comparison transforms only ranges with unambiguous correspondence.
It never assumes a common prefix identifies the edited occurrence. Uncertain
anchors remain uncertain through later recomputation, even when similar text
reappears. Fresh source-to-result passages remain available for navigation.

Mapping values belong to revision snapshots. Undo, redo and checkpoint discard
restore text, decisions and mapping together while assigning the normal fresh
transaction revision. Previously issued previews remain stale. Source captures
stay immutable. These APIs perform no persistence, Qt or filesystem work.
[`EditingCommands`](EditingCommands.md) builds scoped choices on these mappings.
SessionModel owns recovery acknowledgment, and the Qt controller translates
exact editor changes without introducing a second undo history.

Disposable C++ fixtures compare the linear-space implementation with independent
enumeration of every best alignment for binary-line sequences up to six lines
and ternary sequences up to four lines, including UTF-8 and missing final
newlines. The C++20 `-O1` AddressSanitizer/UndefinedBehaviorSanitizer run checked
34,614 source/result pairs and passed 34,630 assertions, including repeated-line
ambiguity, stale anchors, manual-edit preservation and history. A one-shot
allocation failure during initial correspondence passed 14 assertions: the
failed file stayed unreadable with its diagnostic and captures, while a second
file retained its sources, result and correspondence.

A generated 20,000-line conflict with a 1,057,790-byte result completed
construction and a whole-buffer draft edit under a 2,000,000 KiB address-space
cap. The `-O2` fixture process peaked at 44,184 KiB resident memory; construction
took 12,225 ms and the edit took 15,498 ms. A real desktop load of a separate
20,000-line conflict beside a small manually edited file completed under a
3,000,000 KiB cap, with 13 acceptance and cleanup assertions. These are local
observations from that fixture run. The representative measurements and proposed
envelope are in tests/fixtures/performance/evidence.md. Quadratic time remains a
tradeoff. Permanent regression additions are deferred to the wave gate by the
worker contract.
