# Representative workload measurements

`branch_truce_workload_measurements` generates the files in memory and measures them. `tests/performance/Workload.h` lists its parts: the workers, the supervisor that runs them, the report writer and the two profiles. This directory does not store a corpus. The recorded run is `evidence.md`, with the same rows in `evidence.tsv`.

The generator, `tests/fixtures/performance/WorkloadText.h`, builds UTF-8 text with a fixed width of 96 bytes on ordinary lines, a shared `repeat-passage` line every 40 lines, and a `café` line every 100 lines. Correspondence uses three sources. Earlier text is the base. Stage 2, stage 3 and the result each change a different residue class of lines, modulo 25. An adverse long line is one buffer with no newline. The editor workload runs in the real window: an `Application` holds one conflict whose sources and result are those line variants, or three long lines, and the `BranchTruce` `Main` scene shows it. It times selecting the file until the result pane holds the text, is laid out and is drawn, and one letter typed at the start of the result until the store has the new revision and a frame is drawn. The correspondence is built before the load and timed by the correspondence workload. A file over `kLargeFileLines` is confirmed first, as a user would, and a file the product refuses, such as a line over `kMaxLineBytes`, is an explicit failure. Git stores a raw blob with `git hash-object --stdin` and reads it back through `GitAdapter::ReadObject`. Recovery commits that payload through `RecoveryStore::commit`, which fsyncs before it returns.

Scratch directories are `truce-workload-*` under `/var/tmp`, or under `BRANCH_TRUCE_WORKLOAD_SCRATCH` when it names an absolute directory. `/tmp` is often a tmpfs, where a recovery fsync would not represent the disk the application data lives on. The report names the filesystem the run measured.

## Commands

Evidence timings come from a Release binary. The gate test checks that small workloads complete, that excessive Git and recovery actions fail explicitly, and that recorded resource limits agree with the product. It has no latency assertions: shared CI runner speed does not establish a product regression. The evidence profile still measures latency against the budgets below. Bounded worker and CTest deadlines catch hangs.

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

The evidence profile refuses to run without `--report`. The registered check is `performance.workloads`. It runs `--profile gate` under `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`.

## Decision rule

A size enters the proposed envelope only when every repeat completed and the median time is within budget. The proposal keeps the largest measured point that qualifies. A size that was not attempted is not claimed. A timeout, resource kill, truncation, unacknowledged edit, explicit failure or fabricated success stays outside the envelope.

Each metric's budget and stall ceiling are in `kMetrics` in `tests/performance/Workload.h`, and the report prints them. The Git proposal also cannot exceed the product's Git output limit.

The application enforces the envelope as the constants in `src/core/ResourceLimits.h`: `kMaxLineBytes`, `kMaxGitReadBytes` and `kMaxRecoveryPayloadBytes`, and `kLargeFileLines`, above which a file loads only after the user confirms it. The matrix takes those sizes from the header, so it always measures each limit itself, and each over-limit probe sits one byte past its limit. The report compares the envelope it proposes with each constant, and starts with the source revision it measured. The gate reads `evidence.tsv` and fails when the envelope recorded there differs from any of the four constants, so changing a limit or the evidence without the other fails the build until the evidence is measured again.

Larger sizes in a group are skipped after a smaller size misses its metric's stall ceiling. The process is killed at 1536 MiB resident set or at its wall-clock ceiling. A killed run is not given a latency.

## What the profiles cover

The evidence profile repeats representative line counts, adverse long lines, Git reads up to `kMaxGitReadBytes`, and recovery payloads up to `kMaxRecoveryPayloadBytes`. It also runs one identical-buffer correspondence control, a Git read and a Git input one byte over `kMaxGitReadBytes`, a recovery commit with a bad digest, and a recovery commit under `RLIMIT_FSIZE`. The gate profile is the small end of that matrix plus the four failure probes. The committed `evidence.md` predates these sizes and the real-window editor workload: it measured Git reads against an 8 MiB guard, timed its editor rows in a three-pane stand-in scene rather than the application's window, and names no source revision. The limits in `src/core/ResourceLimits.h` rest on that run until the evidence profile is run again.
