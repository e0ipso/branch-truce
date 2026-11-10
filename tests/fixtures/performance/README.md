# Representative workload measurements

`branch_truce_workload_measurements` generates the files in memory and measures them. `tests/performance/Workload.h` lists its parts: the workers, the supervisor that runs them, the report writer and the two profiles. This directory does not store a corpus. The recorded run is `evidence.md`, with the same rows in `evidence.tsv`.

The generator, `tests/fixtures/performance/WorkloadText.h`, builds UTF-8 text with a fixed width of 96 bytes on ordinary lines, a shared `repeat-passage` line every 40 lines, and a `café` line every 100 lines. Correspondence uses three sources. Earlier text is the base. Stage 2, stage 3 and the result each change a different residue class of lines, modulo 25. An adverse long line is one buffer with no newline. The editor loads the same bytes into the result and both source panes of `tests/fixtures/editor/Editor.qml`. Git stores a raw blob with `git hash-object --stdin` and reads it back through `GitAdapter::ReadObject`. Recovery commits that payload through `RecoveryStore::commit`, which fsyncs before it returns.

Scratch directories are `truce-workload-*` under `/var/tmp`, or under `BRANCH_TRUCE_WORKLOAD_SCRATCH` when it names an absolute directory. `/tmp` is often a tmpfs, where a recovery fsync would not represent the disk the application data lives on. The report names the filesystem the run measured.

## Commands

Evidence timings come from a Release binary. The Debug gate test only checks that small workloads complete and that excessive Git and recovery actions are explicit failures.

```sh
cmake -S . -B build/workload-evidence -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON -DBRANCH_TRUCE_BUILD_DESKTOP=ON \
  -DBRANCH_TRUCE_SANITIZERS=OFF
cmake --build build/workload-evidence --target branch_truce_workload_measurements --parallel 8
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ./build/workload-evidence/tests/branch_truce_workload_measurements \
  --profile evidence --report tests/fixtures/performance/evidence.md
```

The registered check is `performance.workloads`. It runs `--profile gate` under `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`.

## Decision rule

A size enters the proposed envelope only when every repeat completed and the median time is within budget. The proposal keeps the largest measured point that qualifies. A size that was not attempted is not claimed. A timeout, resource kill, truncation, unacknowledged edit, explicit failure or fabricated success stays outside the envelope.

Each metric's budget and stall ceiling are in `kMetrics` in `tests/performance/Workload.h`, and the report prints them. The Git proposal also cannot exceed the product's Git output limit.

The application enforces the envelope as the constants in `src/core/ResourceLimits.h`: `kMaxLineBytes`, `kMaxGitReadBytes` and `kMaxRecoveryPayloadBytes`, and `kLargeFileLines`, above which a file loads only after the user confirms it. The matrix takes those sizes from the header, so it always measures each limit itself, and each over-limit probe sits one byte past its limit. The report compares the envelope it proposes with each constant, and starts with the source revision it measured. The gate reads `evidence.tsv` and fails when the envelope recorded there differs from any of the four constants, so changing a limit or the evidence without the other fails the build until the evidence is measured again.

Larger sizes in a group are skipped after a smaller size misses its metric's stall ceiling. The process is killed at 1536 MiB resident set or at its wall-clock ceiling. A killed run is not given a latency.

## What the profiles cover

The evidence profile repeats representative line counts, adverse long lines, Git reads up to `kMaxGitReadBytes`, and recovery payloads up to `kMaxRecoveryPayloadBytes`. It also runs one identical-buffer correspondence control, a Git read and a Git input one byte over `kMaxGitReadBytes`, a recovery commit with a bad digest, and a recovery commit under `RLIMIT_FSIZE`. The gate profile is the small end of that matrix plus the four failure probes. The committed `evidence.md` predates these sizes: it measured Git reads against an 8 MiB guard and names no source revision.

## Accepted controller edit with a QML consumer

`tests/performance/AcceptedEditProbe.cpp` uses the same line generator as the
workload above: 4,000 lines, 375,900 initial result bytes, two distinct source
variants and a third result variant. It creates the real controller, binds a live
QML object to selected-file result text, source text, passages and revision,
and inserts `x` at offset zero ten times. Every sample checks acceptance,
controller and bound result bytes, unchanged source, nonempty passages, and a
new revision. Timing starts before `FileCommands::editResult` and
ends after processing queued Qt events. This includes the old lazy selected-file
map conversion and its QML consumers. Initial construction is excluded.

Configure two existing checkouts into fresh, independent build directories with
matching Debug settings. Build the coordinator test target in each: its link
dependencies include the controller libraries and the Qt-generated controller
resource object in the current tree. Building `branch_truce_controller` alone
does not produce that object. Compile the *same* probe source against each build's generated
coordinator test flags and libraries:

```sh
cmake -S "$BEFORE" -B "$BEFORE_BUILD" -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON -DBRANCH_TRUCE_BUILD_DESKTOP=ON \
  -DBRANCH_TRUCE_SANITIZERS=OFF
cmake -S "$AFTER" -B "$AFTER_BUILD" -G 'Unix Makefiles' \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DBRANCH_TRUCE_WARNINGS_AS_ERRORS=ON -DBRANCH_TRUCE_BUILD_DESKTOP=ON \
  -DBRANCH_TRUCE_SANITIZERS=OFF
cmake --build "$BEFORE_BUILD" --target branch_truce_coordinator_tests --parallel 2
cmake --build "$AFTER_BUILD" --target branch_truce_coordinator_tests --parallel 2
python3 "$AFTER/tests/performance/build-accepted-edit-probe.py" "$BEFORE" \
  "$AFTER/tests/performance/AcceptedEditProbe.cpp" /tmp/accepted-before "$BEFORE_BUILD"
python3 "$AFTER/tests/performance/build-accepted-edit-probe.py" "$AFTER" \
  "$AFTER/tests/performance/AcceptedEditProbe.cpp" /tmp/accepted-after "$AFTER_BUILD"
/tmp/accepted-before
/tmp/accepted-after
```

Use absolute paths for the checkout and build variables. The build directory
argument is optional and defaults to `CHECKOUT/build/desktop`.
Keep the source checkouts and generated builds paired; the helper uses the build's
absolute compiler flags and link inputs.

On the development host, the historical tree `a3a3ff3883471d87f64d81f54c33a8cadaa69a55`
(preceding the selected-file change) and the current tree were measured
serially using Qt 6.10.2 Debug builds. Ten edits measured 1,904.92 ms median
(range 1,897.14–1,909.86) before and 1,826.57 ms median (range
1,818.96–1,872.07) after. These are single-process sequential samples on one
host, not independent process repetitions or a Release performance claim.
The probe has two sources and no real rendered editor; it does not measure
physical desktop interaction or set a performance budget.
