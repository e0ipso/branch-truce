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
support character-boundary byte ranges. A changed passage supports only its
established outer boundaries. Selecting a smaller part of a changed line needs
manual editing rather than a substring search. The policy can reject useful
choices, but it does not invent certainty. Identical buffers take a direct path,
so repeated text with identical positions remains individually addressable.

The algorithm uses O(source lines * result lines) time and O(source lines +
result lines) memory for different buffers, including line, match and passage
records. Each split releases its length rows before recursing. Lines are
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
retain their selected anchor, including a resulting empty range.

The two flags answer different questions. Manual overlap means a choice would
replace work other than the source, so replacing the range needs confirmation.
Binding sets it where the launch text already differs. The hand-edit flag
records editor changes only. A manual edit sets it on every anchor whose range
contains the edit or that the edit crosses, and `replaceDraft` sets it wherever
its comparison changed a range. A choice clears it on each anchor whose whole
range it replaces. A whole-file command clears it on all.

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
