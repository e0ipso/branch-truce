# Supported resource envelope

The enforced limits come from the representative measurements in
[tests/fixtures/performance/evidence.md](../tests/fixtures/performance/evidence.md).
A size is inside the envelope only when the measured repeats completed inside
their latency budget. The next size is an explicit failure. Nothing in that
failure is reported as a successful or truncated result.

| Boundary | Limit | Just above it |
| --- | ---: | --- |
| File | 4000 lines | correspondence is not built; the captured text stays in the session |
| Longest line | 262144 bytes | the edit is refused and the previous buffer stays in place |
| Git process output | 4194304 bytes | `OutputLimit`, with an incomplete captured prefix |
| Recovery payload | 8388608 bytes | the commit fails, the prior journal record stays acknowledged, and the new payload is not published |

Each Git phase, including the canonical blob read and the checkout read, gets
its own 4194304-byte budget. Stdout and stderr share the budget of that phase.
Preflight output does not reduce a later read. A worktree file read stops at the 8388608-byte
recovery ceiling, and a complete read still has to fit the file and line limits
before it becomes editable.

An edit that would cross a file, line, or text-size limit is not acknowledged.
The editor does not load that text. Mark resolved of a result that is already
inside the envelope is unchanged. A recovery commit that exceeds the payload
limit leaves the acknowledged journal record in place and says that pending
input is retained.

## Word-level differences

The panes underline and tint the words that differ inside changed lines
([src/core/WordDifferences.h](../src/core/WordDifferences.h)). These ranges
are display only, so their limits drop the word ranges and never the text:

| Boundary | Limit | Just above it |
| --- | ---: | --- |
| Line length | 1000 bytes, newline excluded | the line is compared as one token and keeps its line colouring only |
| Token comparisons for one pair of passages | 1000000 | the pair gets no word ranges |
| Token comparisons for one revision | 4000000 | pairs later in the text get no word ranges |

The ranges are computed once for each result revision. Changing the theme or
redrawing the panes reuses them.
