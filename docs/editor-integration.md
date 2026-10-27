# Qt Quick editor selection experiment

The initial editor selection experiment chooses **Qt Quick Controls TextArea in plain-text mode**, with a C++
QQuickTextDocument adapter, as the foundation for the merge editor. This keeps the
locked C++20, Qt Quick and Kirigami stack. The standalone experiment imports
Kirigami, uses its theme, and links no product targets.

## Product comparison

The product comparison view integrates that foundation in `src/ui/ComparisonView.qml`. Resizable
left-source, editable-result and right-source panes retain their reading space
when the source-choice controls are hidden. Headers identify the side and display
the captured verified merge or rebase label. Captured blob identities have full
commit/blob details in their tooltip and accessibility description. Sources come
from immutable session captures, while the result comes from the current draft,
initialized with the launch-time working file. Showing earlier-version context,
resizing panes and revealing choices preserve the result and selection. An absent
earlier source is explained; a present empty earlier source remains available.

Every pane has a line-number gutter. The `LineGutter` component sits inside the
pane's Flickable as a sibling of the TextArea, so the result TextArea is still
the Flickable's content child, and the TextArea pads its text by the gutter's
width. `TextBlocks` (`src/ui/TextBlocks.h`) reads block positions from the
QQuickTextDocument, so each line of the file gets one number and wrapped
continuation lines get none. Between the numbers and the text the gutter keeps
a column, `markerSlot`, for per-line glyphs and per-region actions. Its
y coordinates are the editor's: place an item at the y of
`editor.positionToRectangle(position)`, using the gutter's `rows` for the blocks
near the view. Code and numbers use `Kirigami.Theme.fixedWidthFont`, and tab
stops are four average character widths.

Diff colours come from the selected file's `passages` and `regions`. Each line
gets one kind. It is added when the compared text lacks it, changed, conflict
when an undecided region covers it, manual when a hand-edited result region
covers it, or, in the result, markers when it belongs to a conflict-marker
block or is a stray marker line. Where the compared text has lines this pane
lacks, the line boundary gets a gap rule. A
`LineBackgrounds` layer under each TextArea paints them with the
`Kirigami.Theme` roles in `docs/ui-design.md`, spanning whole blocks so wrapped
lines stay covered. It never sets character formats. The `markerSlot` shows the
matching glyph (`+`, `~`, `!`, `m`, and `−` for a gap), and the result gutter
adds a provenance letter: `L`, `R` or `B` for a line equal to the left, right or
both sources, `m` for a hand edit. The same layer highlights the lines of the
navigation target in all panes with `highlightColor`. Source lines come from the
target's region, or else from mapping its result lines through that source's
passages. The highlight stays when the result selection changes.

The result folds the marker lines of each block that `markerBlocks` marks
`folded`: a block of the current revision that `FileSession::markerSections`
ties to the choice sections it encloses, so its marker lines are still the
ones Git wrote.
`TextBlocks.hidden` hides those document blocks with `QTextBlock::setVisible`
and then sets each block's own format, which changes no text but makes the
TextEdit lay it out again. A line is hidden only while it holds the text the
controller listed for it. The buffer, positions and line numbers stay the
document's, so the gutter skips the numbers of folded lines, and Save, recovery
and Mark resolved see the same text either way. A chip with the `!` glyph on
`negativeBackgroundColor` opens each folded block, a rule marks each run of
folded lines, the lines between the markers take the markers kind, and a band
that meets a block is a conflict band. The cursor never rests on a folded line:
a key steps over it, a move by lines keeps its column, and a selection set by
code ends on its anchor's side. A hand edit that changes a folded line's text
or joins it to a neighbour unties its block from its sections, so the block's
lines show; undo restores the text and the fold with it. Show conflict markers
in the file toolbar's overflow menu shows every marker line, and Find or Go to
line on a folded line turns it on. Each region also carries `markersRemain`,
true while a block that encloses its section remains.

The panes highlight syntax with KF6 SyntaxHighlighting. The controller gives
the documents of the left, right, earlier and result panes one `PaneHighlighter`
each (`src/app/PaneHighlighter.h`). It is the only code that sets character
formats on those documents. Their own QML layers draw the line backgrounds and
whitespace markers. The selected file's `fileName` picks the definition through
`Repository::definitionForFileName`, so `app.py` gets Python and `README.md`
Markdown. A name with no definition, such as a `.txt` file, gets no syntax
formats, and its highlighters stay attached so decorations drawn through
`PaneHighlighter::setDecorations` still apply. The view passes
`Kirigami.Theme.backgroundColor` to `setSyntaxBackground` when it loads and
whenever the colour scheme changes. A background with HSL lightness below one
half selects Breeze Dark, any other Breeze Light. A highlight pass changes
formats only. The result's edit capture compares the document text with the
text it last loaded or captured, so the pass adds no revision and no recovery
transaction (`app.syntax-highlighting`).

Selecting a file opens it at its first undecided region. Replacing the document
text leaves the editor's cursor at the end of the file, which would scroll the
result there. The controller moves the cursor to the start of the first
undecided region instead, or to the start of the file when there is none, and
publishes that region as `editorPlacement`. The view puts the region's first
line on a reference row that every pane shares. The row sits three lines below
the top, or a quarter of the shortest pane if that is less. Each file opens as
an unsupported placeholder while its captures load, so the controller places it
again once the loaded file replaces the placeholder.

Scrolling one pane scrolls the others. The view takes the line on the reference
row of the pane the user scrolls and maps it through that side's passages to the
result, then from the result through each other side's passages. Each other pane
scrolls to put its line on the same row. An equal passage maps line by line. Any
other passage maps proportionally between its boundaries, so an ambiguous one
scrolls smoothly instead of jumping. When the leading pane passes lines another
pane lacks, that pane holds still. When the other pane has the extra lines, it
skips them. A side without passages maps proportionally over the whole text, and
the earlier version scrolls with the rest while it is shown. A pane leads only
while the user drags, flicks or wheels it, drags its scroll bar, or has keyboard
focus in its text. A relayout that moves some other pane does not lead. A guard
keeps the panes being moved from leading in turn, so every pane settles in the
same event-loop turn. Find and within-file navigation bring the other panes
along. Rows are not padded to line up, so below the reference row the panes
drift apart wherever one side has extra lines.

The result supports pointer selection, native copy/paste shortcuts, toolbar
copy/paste and a context menu. Undo and redo use the shared text-and-decision
history. The adapter observes completed native key, IME and clipboard operations
as commands, keeping intermediate document notifications out of the journal.
Recovery must commit before the model acknowledges each command. A failed commit
keeps the pending input visible and blocks further edits until recovery is retried.
Source comparisons and view changes do not replace that pending draft.

Permanent comparison regressions are deferred to the wave gate. Focused authoring
checks use disposable Qt scenes and merge/rebase repositories. Product QML changes
also require the full integrated e2e gate. The standalone experiment below remains
useful for component behavior and does not replace product checks.

## Candidates and decision

- TextArea inherits TextEdit's document, selection and input APIs and adds Quick
  Controls styling. The experiment demonstrates the required integration
  boundaries on the installed runtime. It is the selected foundation.
- Bare TextEdit shares those underlying mechanisms. It would need more desktop
  control styling without removing the adapter work. It was assessed through the
  shared API, not a separate comparative performance run.
- KTextEditor offers a mature editor, but its View is a QWidget. A supported Quick
  embedding, event, focus and rendering arrangement has not been demonstrated.
  It is not selected within the locked Quick interface. It was assessed from its
  documented boundary, not installed or benchmarked.

TextArea still needs application-owned gutters, correspondence, navigation,
search controls, input scheduling, pending/error presentation and history. Syntax
highlighting does not establish merge correspondence.

## Reproduce

Run from the repository root using the already-installed dependencies. Build
output and disposable recovery data stay outside the checkout.

```sh
cmake -S experiments/editor-integration -B /tmp/branch-truce-editor-build
cmake --build /tmp/branch-truce-editor-build -j2
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  /tmp/branch-truce-editor-build/editor-experiment --baseline
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  /tmp/branch-truce-editor-build/editor-experiment
/usr/lib/qt6/bin/qmllint "$PWD/experiments/editor-integration/Editor.qml"
```

The root desktop build also registers this fixture as `editor.baseline` and
`editor.regression` in CTest. Run both with
`ctest --test-dir build/desktop -R '^editor\.' --output-on-failure -V`.
The fixture remains independent of product targets and returns nonzero for
failed assertions. The acceptance runner requires
`offscreen` to avoid changing a desktop user's clipboard. Its private clipboard
is cleared, its owner-only recovery directory is removed, and removal is checked.
The build directory can be retained for review and removed afterwards.

## Observed contracts

On Tuxedo OS 24.04.4, GCC 13.3, Qt 6.10.2 and Kirigami 6.24.0, the baseline
reports 8 assertions and the adapted experiment 158 assertions, both with zero
failures. QML lint produces no diagnostics, and the C++ build uses `-Wall
-Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.

Permanent regressions also check failed redo persistence, retrying the same
redo, replacing the redo branch with a new command, rejecting reversed ranges
and unsupported choices without mutation, and acknowledging recovered input
only once across repeated retries.

### Text and positions

The baseline `Aé😀é\n` occupies 11 UTF-8 bytes and 7 editor UTF-16 units. The
nmapping includes the end boundary, round-trips every scalar boundary in both
directions, and rejects interior UTF-8 bytes, split surrogates, unpaired
surrogates, malformed UTF-8 and out-of-range positions. Combining marks remain
separate scalar boundaries; cursor movement may instead follow grapheme clusters.
Joined emoji, Arabic, tabs, repeated passages and a final newline round-trip.
Offsets are logical text positions, independent of wrapping or bidi screen order.

`QTextDocument::toPlainText()` normalizes NBSP to a space, confirmed by the
baseline. The adapter reads `toRawText()` and projects paragraph separators to
LF, preserving NBSP. Literal CR, U+2028 and U+2029 are refused rather than silently
normalized. UTF-8 must decode and re-encode identically, and a scratch document
must round-trip each accepted string. The result conversion layer must handle
CRLF/encoding metadata explicitly; literal separator support needs a lossless
projection before those files can be edited. These are component integration
limits, not permission to silently normalize a repository file.

The command fixture checks a captured revision and exact UTF-8 range before
replacement. A command against a deleted anchor is rejected without searching
for nearby repeated text. Unequal insertions, explicit partial-line fixture
choices and a confirmed insertion of the second side after a manual edit are
reversible. The experiment supplies known ranges and confirmation; it does not
implement stable anchor transformation, insertion affinity, diffing or ambiguity
resolution. Those remain merge-engine responsibilities.

### Composed and basic input

Repeated native `QInputMethodEvent` pre-edit updates change neither committed
text nor acknowledgment count. A commit with a new pre-edit produces exactly one
command, leaves composition active, and cancellation adds none. IME replacement
is captured after the complete event and translated against the prior UTF-8
buffer. Choices are blocked during composition.

Key insertion, Backspace, selection, copy, cut and paste use the real TextArea.
Insertion and deletion are separate acknowledged commands; wrapped paste/cut
operations each produce one command. An event-delivery guard groups intermediate
native notifications and filters formatting/pre-edit noise. Product menus,
shortcuts, drag/drop, middle-click paste and any other mutation path must use the
same transaction boundary. Arbitrary QML edits must not bypass it. This experiment
covers explicit event and menu wrappers, not every desktop input route.

### Application history

Native QTextDocument undo is disabled. Ctrl-Z routes to application history;
redo and menu commands must route there too. The history restores text plus both
contribution decisions in stack order. Ignore changes only its decision. Manual
editing preserves accepted decisions; undoing an explicitly confirmed second-side
combination restores both its prior manual text and decisions. Branching history
truncates redo. The command fixture deliberately has no Git, Save or publishing
operations, and does not persist reopened undo history.

Undo, redo and choices replace the whole document text. For the file already
shown, the controller keeps the cursor, the selection and the result scroll
position across that replacement. Lines the line correspondence keeps equal split
the replacement into separate changed ranges, so text between two changes is part
of neither. Offsets before a changed range stay, offsets after it move with the
text, and offsets inside it go to the end of its new text. An offset at an
insertion point moves past the inserted text, as it does when typing. The
viewport moves only when the cursor would otherwise leave the view.
A newly selected file still opens at its first undecided region.

### Reading aids

The QML scene has labeled read-only sources around the result, external line and
section action labels, and optional earlier-version context. The runner verifies
read-only input refusal, wrapping through increased layout height, scoped search
of a repeated passage, selection preservation when earlier context is hidden,
word highlighting with both background and underline, and QTextOption space/tab
markers. Text and decisions remain unchanged and no history command is added.
Action labels illustrate gutter placement; they are not wired product controls.
The experiment has no aligned rows, real correspondence, scrolling synchronization
or earlier source content. The product comparison view has all of them except
aligned rows. Search here uses the selected document only.

### Recovery acknowledgment

Initial persistence failure keeps editing disabled. Successful native input is
visible before persistence but is acknowledged only after the sink accepts the
complete text-and-decisions state. Failed persistence freezes the visible pending
buffer, exposes a pending status, leaves acknowledgment/history unchanged, and
blocks further edits, choices and undo. A new reader restores the previous
acknowledged state. Retry persists and acknowledges the retained input exactly
once. A failed choice or undo does not apply it. An unavailable directory exercises
real I/O failure separately from injected transaction failure; corrupt payloads
are rejected. No repository files are touched.

The disposable sink uses a versioned JSON envelope, SHA-256, owner-only permissions
and QSaveFile with direct-write fallback disabled. The observed run performed 26
snapshot writes with a maximum of 116526 ns, a single local sample, not a latency
budget or crash-durability result. It persists entire states to make ordering
observable. It is **not** the ordered recovery journal required by TECH_SPEC.md.
Source/launch identity validation, payload bounds, complete-record handling,
explicit Save checkpoints, durable discard, crash injection and publication
recovery still need implementation and verification. Recovery status always says
unsaved; it implies neither review nor Git resolution.

## Integration and platform gates

New committed regression tests are deferred to the wave gate under the worker
contract; these local acceptance fixtures remain reproducible. Add coverage for
all native mutation routes.
Representative editor, correspondence, Git and recovery measurements, including long lines, are in tests/fixtures/performance/evidence.md. The enforced envelope is documented in [resource limits](resource-limits.md). Replace full-buffer snapshots,
linear offset scans and whole-text rerendering before production-scale use.
Implement acknowledgment queuing and normal-close/file-switch behavior, including
explicit disposition of pending composition. Do not infer support for arbitrary
text merely from a successful QString conversion.

This run used Qt's software offscreen platform. Synthetic IME events validate the
Qt event API, not a physical keyboard, IBus/Fcitx candidate window or real input
context. Kubuntu 26.04 and Tuxedo OS Wayland sessions still need physical composed
input, clipboard/selection, pointer selection, scrolling, bidi hit testing,
scaling, Breeze/dark theme and accessibility checks. No installed components were
changed. The standalone experiment changed no shared product render markup. The
subsequent product comparison requires the full integrated e2e gate and retains
the platform obligations.

## Primary API references

- [TextEdit](https://doc.qt.io/qt-6/qml-qtquick-textedit.html) documents the inherited
  editing, composition, selection and plain-text APIs.
- [QQuickTextDocument](https://doc.qt.io/qt-6/qquicktextdocument.html) exposes the
  underlying document for C++ integration and formatting.
- [QTextDocument](https://doc.qt.io/qt-6.10/qtextdocument.html#toRawText) documents
  raw extraction, plain-text normalization and native undo control.
