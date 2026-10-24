# Representative workload measurements

`branch_truce_workload_measurements` generates the files in memory and measures them. This directory does not store a corpus. The recorded run is `evidence.md`, with the same rows in `evidence.tsv`.

The generator builds UTF-8 text with a fixed width of 96 bytes on ordinary lines, a shared `repeat-passage` line every 40 lines, and a `café` line every 100 lines. Correspondence uses three sources. Earlier text is the base. Stage 2, stage 3 and the result each change a different residue class of lines, modulo 25. An adverse long line is one buffer with no newline. The editor loads the same bytes into the result and both source panes of `experiments/editor-integration/Editor.qml`. Git stores a raw blob with `git hash-object --stdin` and reads it back through `GitAdapter::ReadObject`. Recovery commits that payload through `RecoveryStore::commit`, which fsyncs before it returns.

Scratch directories are `/var/tmp/truce-workload-*`. `/tmp` is tmpfs on the development host, so a recovery fsync there would not represent the ext4 application-data disk.

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

Budgets are correspondence construction 1000 ms, correspondence edit 1000 ms, editor load 1000 ms, editor keystroke 100 ms, Git read 2000 ms, and recovery acknowledgment 500 ms. The Git proposal also cannot exceed the existing 8 MiB output guard. That guard is not changed by the measurement. TRUCE-0029 enforces the resulting envelope: 4000 lines, 262144-byte lines, 4194304-byte Git reads, and 8388608-byte recovery payloads.

Larger sizes in a group are skipped after a smaller size misses a stall ceiling: 4000 ms for correspondence and editor load, 400 ms for a keystroke, 8000 ms for a Git read, and 2000 ms for recovery. The process is killed at 1536 MiB resident set or at its wall-clock ceiling. A killed run is not given a latency.

## What the profiles cover

The evidence profile repeats representative line counts, adverse long lines, Git reads through 8 MiB, and recovery payloads through 8 MiB. It also runs one identical-buffer correspondence control, a Git read above 8 MiB, a Git input above 8 MiB, a recovery commit with a bad digest, and a recovery commit under `RLIMIT_FSIZE`. The gate profile is the small end of that matrix plus the four failure probes.
