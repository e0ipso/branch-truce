# Workload measurements

Build type Release. Compiler 13.3.0. Qt 6.10.2. Git git version 2.43.0.

Machine Linux 7.0.0-111030-tuxedo x86_64, 11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz, 8 processors, MemTotal:       16104872 kB. Scratch filesystem ext4   /dev/mapper/system-root.

Editor samples use QT_QPA_PLATFORM=offscreen and QT_QUICK_BACKEND=software. They are not a Wayland or KDE session. Recovery and Git scratch live under /var/tmp because /tmp is tmpfs on this host.

## Decision rule

A completed sample can enter the proposed envelope only when every repeat completed and the median elapsed time is within the budget. The proposed size is the largest measured point that qualifies. Sizes that were not attempted are not claimed. Timeouts, resource kills, truncation, unacknowledged edits, explicit failures and fabricated success stay outside the envelope.

Budgets are correspondence construction 1000 ms, correspondence edit 1000 ms, editor load 1000 ms, editor keystroke 100 ms, Git read 2000 ms, and recovery acknowledgment 500 ms. The Git figure cannot exceed the existing 8 MiB output guard.

## Results

| Name | Outcome | Bytes | Lines | Max line | Samples | Min ms | Median ms | Max ms | RSS KiB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| correspondence-lines-200-construct | completed | 18795 | 200 | 96 | 3 | 1 | 1 | 1 | 27840 |
| correspondence-lines-200-edit | completed | 18795 | 200 | 96 | 3 | 1 | 1 | 1 | 27936 |
| correspondence-lines-1000-construct | completed | 93975 | 1000 | 96 | 3 | 28 | 28 | 41 | 29452 |
| correspondence-lines-1000-edit | completed | 93975 | 1000 | 96 | 3 | 27 | 28 | 28 | 29684 |
| correspondence-lines-2000-construct | completed | 187950 | 2000 | 96 | 3 | 112 | 112 | 112 | 31180 |
| correspondence-lines-2000-edit | completed | 187950 | 2000 | 96 | 3 | 111 | 111 | 112 | 31508 |
| correspondence-lines-4000-construct | completed | 375900 | 4000 | 96 | 3 | 450 | 450 | 450 | 35112 |
| correspondence-lines-4000-edit | completed | 375900 | 4000 | 96 | 3 | 449 | 450 | 450 | 35864 |
| correspondence-lines-8000-construct | completed | 751800 | 8000 | 96 | 3 | 1803 | 1813 | 1814 | 42720 |
| correspondence-lines-8000-edit | completed | 751800 | 8000 | 96 | 3 | 1799 | 1801 | 1803 | 44112 |
| correspondence-lines-16000-construct | completed | 1503600 | 16000 | 96 | 3 | 7221 | 7222 | 7224 | 58172 |
| correspondence-lines-16000-edit | completed | 1503600 | 16000 | 96 | 3 | 7206 | 7208 | 7209 | 60924 |
| correspondence-long-4096-construct | completed | 4096 | 1 | 4096 | 3 | 0 | 0 | 0 | 27480 |
| correspondence-long-4096-edit | completed | 4096 | 1 | 4096 | 3 | 0 | 0 | 0 | 27560 |
| correspondence-long-65536-construct | completed | 65536 | 1 | 65536 | 3 | 0 | 0 | 0 | 28580 |
| correspondence-long-65536-edit | completed | 65536 | 1 | 65536 | 3 | 0 | 0 | 0 | 28776 |
| correspondence-long-1048576-construct | completed | 1048576 | 1 | 1048576 | 3 | 7 | 11 | 12 | 46908 |
| correspondence-long-1048576-edit | completed | 1048576 | 1 | 1048576 | 3 | 5 | 7 | 8 | 48976 |
| correspondence-long-8388608-construct | completed | 8388608 | 1 | 8388608 | 3 | 69 | 70 | 70 | 183108 |
| correspondence-long-8388608-edit | completed | 8388608 | 1 | 8388608 | 3 | 46 | 46 | 46 | 199444 |
| correspondence-identical-4000-construct | completed | 375900 | 4000 | 96 | 1 | 3 | 3 | 3 | 34376 |
| correspondence-identical-4000-edit | completed | 375900 | 4000 | 96 | 1 | 462 | 462 | 462 | 35472 |
| editor-startup | completed | 0 | 0 | 0 | 3 | 65 | 75 | 79 | 59472 |
| editor-lines-200-load | completed | 18795 | 200 | 96 | 3 | 13 | 13 | 16 | 60620 |
| editor-lines-200-key | completed | 18795 | 200 | 96 | 3 | 0 | 0 | 0 | 60744 |
| editor-lines-2000-load | completed | 187950 | 2000 | 96 | 3 | 104 | 107 | 107 | 66140 |
| editor-lines-2000-key | completed | 187950 | 2000 | 96 | 3 | 0 | 0 | 0 | 66288 |
| editor-lines-10000-load | completed | 939750 | 10000 | 96 | 3 | 526 | 528 | 531 | 90920 |
| editor-lines-10000-key | completed | 939750 | 10000 | 96 | 3 | 3 | 3 | 3 | 90920 |
| editor-lines-20000-load | completed | 1879500 | 20000 | 96 | 3 | 1067 | 1073 | 1076 | 121536 |
| editor-lines-20000-key | completed | 1879500 | 20000 | 96 | 3 | 10 | 10 | 13 | 121536 |
| editor-long-1024-load | completed | 1024 | 1 | 1024 | 3 | 0 | 0 | 0 | 59664 |
| editor-long-1024-key | completed | 1024 | 1 | 1024 | 3 | 0 | 0 | 0 | 59852 |
| editor-long-8192-load | completed | 8192 | 1 | 8192 | 3 | 10 | 11 | 13 | 61356 |
| editor-long-8192-key | completed | 8192 | 1 | 8192 | 3 | 1 | 1 | 3 | 61708 |
| editor-long-65536-load | completed | 65536 | 1 | 65536 | 3 | 69 | 70 | 70 | 70860 |
| editor-long-65536-key | completed | 65536 | 1 | 65536 | 3 | 23 | 23 | 23 | 73380 |
| editor-long-262144-load | completed | 262144 | 1 | 262144 | 3 | 281 | 284 | 297 | 110496 |
| editor-long-262144-key | completed | 262144 | 1 | 262144 | 3 | 89 | 89 | 109 | 112740 |
| editor-long-1048576-load | completed | 1048576 | 1 | 1048576 | 3 | 1178 | 1193 | 1197 | 255676 |
| editor-long-1048576-key | completed | 1048576 | 1 | 1048576 | 3 | 387 | 394 | 395 | 267968 |
| editor-long-4194304-load | explicit-failure | 4194304 | 1 | 4194304 | 3 | - | - | - | 795492 |
| editor-long-4194304-key | incomplete | 4194304 | 1 | 4194304 | 3 | - | - | - | 795492 |
| git-read-65536 | completed | 65536 | 1 | 65536 | 3 | 14 | 14 | 15 | 30756 |
| git-read-1048576 | completed | 1048576 | 1 | 1048576 | 3 | 15 | 15 | 16 | 33044 |
| git-read-4194304 | completed | 4194304 | 1 | 4194304 | 3 | 21 | 24 | 24 | 42280 |
| git-read-8388608 | explicit-failure | 8388608 | 1 | 8388608 | 3 | - | - | - | 46704 |
| git-read-over-limit | explicit-failure | 8392704 | 1 | 8392704 | 1 | - | - | - | 46864 |
| git-input-over-limit | explicit-failure | 8388609 | 1 | 8388609 | 1 | - | - | - | 37200 |
| recovery-4096-commit | completed | 4096 | 1 | 4096 | 3 | 14 | 15 | 15 | 29140 |
| recovery-4096-reload | completed | 4096 | 1 | 4096 | 3 | 0 | 0 | 0 | 29244 |
| recovery-65536-commit | completed | 65536 | 1 | 65536 | 3 | 16 | 16 | 17 | 29168 |
| recovery-65536-reload | completed | 65536 | 1 | 65536 | 3 | 1 | 1 | 1 | 29336 |
| recovery-1048576-commit | completed | 1048576 | 1 | 1048576 | 3 | 44 | 44 | 46 | 31108 |
| recovery-1048576-reload | completed | 1048576 | 1 | 1048576 | 3 | 17 | 17 | 25 | 32252 |
| recovery-4194304-commit | completed | 4194304 | 1 | 4194304 | 3 | 87 | 87 | 96 | 37264 |
| recovery-4194304-reload | completed | 4194304 | 1 | 4194304 | 3 | 68 | 68 | 68 | 41480 |
| recovery-8388608-commit | completed | 8388608 | 1 | 8388608 | 3 | 163 | 164 | 165 | 45504 |
| recovery-8388608-reload | completed | 8388608 | 1 | 8388608 | 3 | 137 | 137 | 137 | 53816 |
| recovery-bad-digest | explicit-failure | 64 | 1 | 64 | 1 | - | - | - | 28940 |
| recovery-overload | explicit-failure | 1048576 | 1 | 1048576 | 1 | - | - | - | 29952 |

## Proposed envelope for TRUCE-0029

Component bounds before taking the tightest measured point: correspondence construction 4000 lines, correspondence edit 4000 lines, editor load 10000 lines, editor keystroke 20000 lines, long-line editor load 262144 bytes, long-line editor keystroke 262144 bytes, long-line correspondence construction 8388608 bytes, long-line correspondence edit 8388608 bytes, Git read 4194304 bytes, recovery commit 8388608 bytes, recovery reload 8388608 bytes.

Representative files, three sources, width 96 bytes including the newline: up to 4000 lines.

Adverse long lines, three panes or three sources: up to 262144 bytes on the longest line.

Git process output: up to 4194304 bytes, and not above the existing 8 MiB guard. The guard was not changed. The 8388608-byte blob did not complete: explicit-failure. api=OutputLimit incomplete=1 captured=8387891 expected=8388608 exit=-1 git_rss_kb=4388 diagnostic=Git output exceeded 8 MiB; captured prefix is incomplete. 

Recovery acknowledgment, commit through fsync plus a following reload: up to 8388608 payload bytes.

Completed samples over the latency budget stay outside the envelope:

- correspondence-lines-8000-construct median 1813 ms, budget 1000 ms
- correspondence-lines-8000-edit median 1801 ms, budget 1000 ms
- correspondence-lines-16000-construct median 7222 ms, budget 1000 ms
- correspondence-lines-16000-edit median 7208 ms, budget 1000 ms
- editor-lines-20000-load median 1073 ms, budget 1000 ms
- editor-long-1048576-load median 1193 ms, budget 1000 ms
- editor-long-1048576-key median 394 ms, budget 100 ms

Identical-buffer correspondence is a direct path and is excluded from the line-count proposal. One failed or skipped probe means that part of the envelope is not established.

## Outcomes outside the envelope

- editor-long-4194304-load explicit-failure. loaded=4194304 expected=4194304 width=0 height=19 text retained but content width is not positive
- editor-long-4194304-key incomplete. skipped after the load was not completed
- git-read-8388608 explicit-failure. api=OutputLimit incomplete=1 captured=8387891 expected=8388608 exit=-1 git_rss_kb=4388 diagnostic=Git output exceeded 8 MiB; captured prefix is incomplete
- git-read-over-limit explicit-failure. api=OutputLimit incomplete=1 captured=8387891 expected=8392704 exit=-1 git_rss_kb=12056 diagnostic=Git output exceeded 8 MiB; captured prefix is incomplete
- git-input-over-limit explicit-failure. api=Rejected incomplete=0 captured=0 expected=8388609 exit=-1 git_rss_kb=- diagnostic=Invalid operands for the named Git operation
- recovery-bad-digest explicit-failure. acknowledged=0 transactions=0 incompleteTail=0 error=Invalid recovery payload identity.
- recovery-overload explicit-failure. acknowledged=0 transactions=0 incompleteTail=0 error=Could not commit a recovery payload; pending input is retained.

## Limitations

Each evidence size was repeated three times on one host. The median is not a population statistic. Repeats can hit the page cache. The default developer build is Debug. These numbers come from the Release binary. Editor latency does not include a physical keyboard, input method, or KDE compositor. Correspondence cost is quadratic in line count. A long line is interned once and is not the quadratic case. RSS is the process high-water mark and later work in the same process can only raise it. Cleanup: scratch directories removed
