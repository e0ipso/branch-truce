# Journey acceptance checks

`JourneyAcceptanceTests.cpp` checks the observable acceptance examples in `PRD.md` through the real application. Each journey creates a disposable Git repository, stops a real merge or rebase on conflicts, opens it with `ApplicationController::openRepository` and loads the real `BranchTruce` `Main` scene. The test then drives that scene with mouse clicks and key events and compares what it shows with the repository and the Git index.

## Running it

From a configured desktop build:

```sh
cmake --build build/desktop --parallel 2 --target branch_truce_journey_acceptance_tests
ctest --test-dir build/desktop -L acceptance --output-on-failure
```

The CTest name is `acceptance.journeys`, with the label `acceptance`. CTest sets `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`. To run the binary directly, set both variables yourself. It exits with status 2 on any other platform plugin. The suite took 28 to 31 seconds on the development host at a load average of 4, and its CTest timeout is 120 seconds.

The process sets up its own environment before Qt starts. It uses a private `HOME`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME` and an owner-only `XDG_RUNTIME_DIR` under one temporary directory, so the recovery store and the mergetool socket never touch the real ones. `PATH` starts with a `git` wrapper that logs every argument the app passes and refuses `fetch`, `pull`, `push`, `clone`, `ls-remote`, `remote`, `submodule` and `credential`. Each fixture repository has an unreachable `origin` remote, and the HTTP proxy variables point at a closed local port. The fixtures call `/usr/bin/git` directly, so the log holds only the app's commands.

`tests/fixtures/QuickScene.h` holds the offscreen helpers that this suite shares with `tests/app/ComparisonViewTests.cpp`. They are `settle`, `visualItem`, `waitForStable`, `waitForLayout`, `clickItem` and the wrap and whitespace probes.

## PRD coverage

| Journey (test name prefix) | PRD and TECH_SPEC lines | What it checks |
| --- | --- | --- |
| the full inventory keeps truthful sources | PRD.md:125, :127, :139, :157; TECH_SPEC.md:224 | Three supported files and one modify/delete file stay listed. The outside-app file keeps its reason and the session warning after all three results are saved. Labels show `Current checkout: <sha>` and `Merged commit: <sha>`, and the captured blob IDs and texts match index stages 2 and 3. The left source, editable center and right source sit side by side. The initial result, including an edit made before launch, matches the file on disk. Earlier context can be shown and hidden without changing the result. Save leaves the repository byte-for-byte unchanged. |
| rebasing keeps the arrangement | PRD.md:139 | Rebase labels read `Current rebased state: <sha>` and `Commit being replayed: <sha>` over the same three-pane layout. An add/add file explains that earlier context is unavailable and shows no empty version. |
| scoped choices and ordered combinations | PRD.md:157 (with :129, :131, :151); TECH_SPEC.md:80 | A choice that would replace pre-launch content opens a preview, and Cancel changes nothing. Accept on one line leaves its competitor undecided. Resolve using one side ignores the other side. Ignore changes decisions but not text. A Right-then-Left combination is previewed in that order before it is applied. Typing goes into the same history. The Undo and Redo buttons and Ctrl+Z / Ctrl+Shift+Z walk every step back and forward, restoring text and decisions together. Other files keep their revision. |
| Save keeps an app-only draft and Mark resolved | PRD.md:135, :153; TECH_SPEC.md:224 | Save marks the draft saved, not reviewed and not resolved, and leaves the repository unchanged. Mark resolved with undecided choices asks for confirmation, and Cancel keeps the text, choices and repository. Confirming without a prior Save writes the displayed bytes and stages them as the blob of the written file. Editing again clears the review and shows that an earlier result is staged and the current draft is not applied. |
| scoped search, wrapping, whitespace | PRD.md:137, :141 | Find in the left source walks repeated passages and wraps back to the first. The search scope is changed with the mouse and the search moves to the result. Wrapping changes the visual line count of a long line. Space and tab markers are painted for the whitespace-only difference. Typing into a source does nothing. Shift+End and Ctrl+C copy a selection, and Ctrl+V, Ctrl+Z, Ctrl+Shift+Z and the Undo button act on the result. None of the reading aids change the text, the decisions or the undo history. |
| the handoff separates resolved, saved, unsaved, newer, failed and outside-app work | PRD.md:141, :143, :145; TECH_SPEC.md:224 | A write failure into a read-only directory keeps the saved draft and leaves the file unresolved. A failing `post-index-change` hook leaves the file shown as "Written, unresolved". File rows state their progress in words. The handoff reports 6 of 7 files outstanding and gives each file its category and blockers, and each route button returns to its file. Leaving after the warning runs no Git command, leaves the repository unchanged and keeps every unfinished path unmerged in Git. |
| offline startup without an account resumes drafts | PRD.md:149 (with :147) | Closing the window opens the handoff. After a deliberate leave, a new controller restores the saved draft and the draft that was never saved, keeps their status and still accepts edits. |

Regression journeys outside the PRD table:

- `TRUCE-0087 a resolved mergetool handoff drops the waiting text` runs real `git mergetool` on one of two paths. Git starts this test binary with `--mergetool`, and `main()` then runs the real launcher code against the session in the test process. While Git waits, the path it has not launched says "Waiting for Git to launch this path. It is not resolved." After Mark resolved, Git exits 0 and stages the result. The resolved file then shows no waiting text, and neither does the same file after a later edit leaves the earlier result staged. The other path still shows the text.
- `TRUCE-0088 a 1280x720 logical screen fits the window` opens a merge with four files and the outside-app warning, then resizes the window to 1280x720 and 960x540, the logical sizes of a 1920x1080 screen at 150% and 200%. Every named control and all three text panes must lie inside the window once enclosing views have scrolled to them, each pane must be at least eight lines high, and the mouse wheel must scroll the page at 960x540. At 1600x1200 the page must not scroll. These are logical sizes in the offscreen platform, not a scaled output; `docs/platform-validation.md` records the scaled container runs.

Every journey also runs `checkOffline`. It asserts that the guard saw no network command, that the app ran local `rev-parse` and `cat-file`, and that no visible text in the scene mentions signing in, logging in, registering, an account, a subscription, a payment or a license key.

PRD examples 4 (whole-file and bulk choices), 11 (crash recovery) and 15 (merge-tool cancellation) are outside this ticket. The suite does not cover them.

## Environment-dependent gaps

- The suite uses the offscreen platform with the software Quick backend. It does not check a real Wayland or X11 session, KDE themes, scaling, input methods on a physical keyboard, accessibility metadata seen by assistive technology, or the colours of focus and progress indicators. TECH_SPEC.md:227 leaves those to packaged-application checks on the target desktops.
- A `git` wrapper on `PATH` plus proxy variables stands in for having no connection. No network namespace is involved, so a direct socket opened by Qt or a library would not be caught.
- Scrolling inside the handoff dialog and the scoped-choice list sets the view position directly instead of sending wheel events. The search scope is changed with real clicks on the combo box and its popup.
- The write-failure journey makes a directory read-only. Root ignores directory permissions, so that step fails when the suite runs as root.
- A failed draft save, a file that cannot open, and leave refused because of pending input or a running resolution need fault injection, so this suite does not provoke them. They are covered in `tests/app`: `TRUCE-0015 Save and discard controls keep the app baseline and retry a failed Save`, `TRUCE-0053 failed placeholder recovery keeps captured files visible blocks editing and retries the captured state`, and `TRUCE-0074 leave refuses unrecovered input and a running resolution and finish waits for a clean handoff`.
- With Git's raw conflict output as the result, Accept and Resolve using this side change only decisions, because both sides' lines are already in the result. Known conflict markers then block Mark resolved. The choice journey starts from a result that the user had set to the common version before launch, so the text changes it checks can be seen.
