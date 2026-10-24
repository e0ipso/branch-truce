# Passage correspondence and revisioned anchors

`CorrespondenceAlgorithm` separates comparison from session bookkeeping. The
default `ExactLineCorrespondence` uses exact UTF-8 lines, including their newline
bytes, without whitespace folding. It computes the longest common subsequence
lengths, then walks every optimal alignment through the dynamic-programming
graph. A line is equal only when every optimal alignment matches it to the same
result line. Other possible matches make the enclosing gap ambiguous. Gaps
without possible matches describe changed, source-only or result-only text.
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

The algorithm uses O(source lines * result lines) time and matrix memory for
different buffers, plus line and passage records. Lines are interned before the
matrix calculation, so long-line bytes are compared during interning rather
than at each matrix cell. Allocation or representation failure throws before a
new session revision is published. There is no selected file-size, latency or
memory limit. Generated-fixture observations help evaluate the implementation;
measurements on representative user files and alternative algorithms remain a
wave-gate obligation.

`SourceAnchor` carries the caller's stable contribution ID, immutable source
side/range and insertion affinity. `ResultAnchor` adds captured session, path and
source-object identities, the exact transaction revision, mapping safety and
manual-overlap state. Contribution IDs cannot be rebound to different source
ranges. `FileSession::replaceMapped` checks the current registered anchor and
revision before changing either text or the contribution's decision. It changes
only that result range, leaving neighboring edits and other decisions intact.
An overlapping manual change requires confirmation for that exact preview.
An ambiguous or invalidated anchor remains unavailable even with confirmation.

`editResult` takes an exact half-open UTF-8 byte edit. Insertions at either edge
of a nonempty anchor stay outside it; an empty anchor explicitly chooses before
or after. Interior edits transform the range and retain a manual-overlap flag.
Deleting a complete target or crossing its boundary invalidates it. Automated
replacements retain their selected anchor, including a resulting empty range.

The existing whole-buffer `replaceDraft` API has no editor edit location. Its
old-to-new comparison transforms only ranges with unambiguous correspondence.
It never assumes a common prefix identifies the edited occurrence. Uncertain
anchors remain uncertain through later recomputation, even when similar text
reappears. Fresh source-to-result passages remain available for navigation.

Mapping values belong to revision snapshots. Undo, redo and checkpoint discard
restore text, decisions and mapping together while assigning the normal fresh
transaction revision. Previously issued previews remain stale. Source captures
stay immutable. These APIs perform no persistence, Qt or filesystem work;
command composition, recovery serialization and editor integration are separate
owners.

The ticket's local evidence uses disposable C++ fixtures, including an independent
enumeration of all best alignments for binary-line sequences up to four lines,
and checks for repeated/changed/inserted/deleted passages, stale and foreign
anchors, UTF-8 boundaries, manual-edit preservation, history and affinity.
Permanent regression additions are deferred to the wave gate by the worker
contract.

The local C++20 `-O1` AddressSanitizer/UndefinedBehaviorSanitizer fixture run
passed 22 cases and 12,381 assertions. Its independent oracle covered 961 source
and result pairs; another 172 edit-position cases checked preservation of an
unaffected anchor's text. Generated unique-line files with one inserted line
took 611, 26,217 and 97,974 microseconds for 100, 1,000 and 2,000 source lines
respectively. Source sizes were 790, 8,890 and 18,890 bytes. A changed 256 KiB
single line remained one changed passage. Maximum resident memory for the
entire sanitized fixture process was 140,008 KiB, including sanitizer overhead
and its other cases. These are observations from one local run, not supported
budgets or measurements of representative user files. The repeated-line oracle
supports retaining only forced matches instead of committing an arbitrary LCS
tie-break; the measured growth keeps the quadratic cost an explicit tradeoff.
