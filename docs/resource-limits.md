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

## Typing and scrolling in highlighted panes

The ctest `performance.highlighted-editor`, under the `performance` and `ui`
labels, opens a 200-line C++ file with a conflict every 20 lines in the real
comparison view, with syntax highlighting and word differences on. It types
six letters into the result and turns the mouse wheel fifteen notches over it,
three times, and measures the process CPU time from each key press or notch to
the next rendered frame. A key that never reaches the document or a notch that
never moves the view fails the run instead of counting as a slow sample. The
median of the three repeat medians must stay within these limits:

| Boundary | Limit | Measured on the development host |
| --- | ---: | --- |
| Typing, one key press to the next frame | 200 ms CPU | 63 to 66 ms alone, 62 to 89 ms with three runs at once, 100 to 106 ms with six at once |
| Scrolling, one wheel notch to the next frame | 300 ms CPU | 100 to 112 ms alone or three runs at once, 154 to 163 ms with six at once |

The runs use the offscreen platform, the software renderer and the
`org.kde.desktop` style on an 8-thread host. For scrolling the load average was
2.0 to 2.7 alone and with three runs, and reached 9.6 with six. The typing row
comes from later runs, made while other builds and tests shared the host. Their
one-minute load average was 3.5 to 6.9 alone, 2.7 to 4.8 with three runs and 4.8
to 7.0 with six. With the Fusion style, typing cost 62 to 72 ms alone. CPU time
leaves out the time other processes hold the CPU, but sharing cores still raised
the medians about 1.6 times. The limits hold at that load. On an idle host they
catch a cost about 3 times the measured typing and scrolling costs, not a
doubling.

A key press costs about 65 ms of CPU here, under the 100 ms editor keystroke
budget in the evidence above. The largest parts are the edit itself, about
10 ms, and its recovery journal commit. The commit reads back every earlier
record of the journal, so it grows with each key: 3 to 5 ms for the first key of
a run and about 25 ms for the eighteenth.
