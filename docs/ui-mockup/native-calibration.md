# Native capture calibration

The approved PNGs and HTML are immutable targets. Their hashes, reference regions,
requirements and narrow exceptions are in
[reference-contract.json](../../tools/native-replay/reference-contract.json).
The measured environment is pinned by
[capture-profile.json](../../tools/native-replay/capture-profile.json).
Use the [native replay](../../tools/native-replay/README.md) for later before/after
captures in both Breeze Light and Breeze Dark. A profile mismatch fails capture.

The PNGs decode to **1920×1080** although the HTML viewport is **1280×720**.
Their effective raster scale is 1.5. This observation does not establish the original
browser's device settings. The current native capture is 1280×720 at observed DPR 1.
Region coordinates in the contract are logical; reports retain the physical reference
crop and native crop separately. Overlays and absolute-channel differences replicate
reference pixels twice and native pixels three times onto a common grid. There is no
interpolation, registration, threshold or pixel mask. Counts describe cells of this
comparison grid, not original image pixels or a similarity percentage. Supplemental
1280×800 full-window differences retain the extra native extent; region comparisons
use actual native footer/pane coordinates rather than stretching the 720-high target.
Original PNGs are copied byte-for-byte beside the derivatives, and close-ups preserve
original pixel dimensions. No derivative replaces an approved reference or native shot.

The current environment is a private headless sway compositor with pixman, Qt Quick's
software backend, Wayland, org.kde.desktop Quick Controls, Breeze icons and Breeze
fallback icons. This is not evidence of a physical Plasma session. Each snapshot records
actual Qt version, OS release, configured colour-scheme name, observed Qt palette,
original kdeglobals colour groups and their file hash, output scale, logical size,
logical/physical DPI and DPR.
The observed typography is Noto Sans 10 pt, Hack 9 pt, and Kirigami smallFont Noto Sans
8 pt. Font resolution reports exactMatch truthfully, including false for proportional
fonts. The environment variables that identify KDE do not prove a desktop session.

`configuredColourScheme` records the name read from kdeglobals; it does not establish
which colours the application uses. `palette` records the actual application palette.
The profile pins its Window, Base, AlternateBase, Text, disabled Text and Highlight
roles separately for Breeze Light and Breeze Dark, with the retained baseline state
hashes. Capture rejects missing or changed observed roles even when the configured
name matches the requested scheme. These observations belong to the controlled
headless profile and do not establish physical-desktop compatibility.

`build-provenance.json` records the executable SHA, build-cache/compile-command hashes,
observed compiler/build type, observed source/build checkout identities and whether
their src bytes match. The executable hash identifies the artifact. An observed checkout
HEAD is not an embedded or independently attested build revision. The tool does not
rebuild or mutate the application. Each state also identifies the preload driver,
profile, contract, fixture and screenshot by SHA. The final verification manifest hashes
capture inputs and retained artifacts; `calibration.py --verify OUTPUT` rejects changes.

| Measure | Native baseline | HTML observation | Governing rule |
| --- | --- | --- | --- |
| Row pitch | 18 logical px | 20 CSS px | Fixed-font line spacing plus smallSpacing; pane budget and scrolling must use the same pitch |
| Header inset | 8 | 12 | largeSpacing, currently 8; no literal 12 px repair |
| Sidebar | 252 | 262 | 14 gridUnits, currently 18 per unit; retain resizing |
| File toolbar | 34 | 40 | Native ActionToolBar row; list/file header hairlines must align |
| Footer | 30 | 28 | Actual smallFont, theme spacing and native frame/separator allowances; inspect extra padding rather than force a height |

SessionFooter sizes its row from the contents. Its ProgressBar sets a preferred width
but leaves native implicit height in charge; the installed desktop style includes
largeSpacing top/bottom insets. The brief instead specifies smallSpacing height for
this indicator. Those native insets, row margins and separator explain why the footer
height needs its own layout review. They are not a fill-shade exception.

These numbers describe a baseline, not new hardcoded UI dimensions. The approved
HTML gives the list header 38 px and file header 40 px. The written aligned-boundary
rule wins over that discrepancy. The native list boundary currently aligns with its
34 px file toolbar. Geometry and type weight remain reviewable independently of colour.

In Breeze Light the observed pane header is #e3e5e7, matching the scheme's Window
alternate, while the application View alternate is #f7f7f7. In Breeze Dark the header
is #292c30, matching Window alternate, while View alternate is #1d1f22. PaneHeader uses
its inherited alternateBackgroundColor; this inheritance explains the shade difference.
It is outstanding against the brief's View-set requirement. The comparison container's
schemeBackground also inherits the Window base. The observer changes no theme roles.
Only the brief's line-fill shade exception applies: background roles win over HTML's
14% light/24% dark mixing of text roles. It cannot excuse pane bases, headers or syntax.
The contract identifies a small glyph-free accepted-row patch rather than exempting a pane.

Syntax observations come from the real QTextDocument format ranges, with line, offset,
length, foreground and weight. SyntaxHighlighting chooses Breeze Light/Dark from
schemeBackground lightness. For example, the light preprocessor is #006e28 and native
include strings are #ff5500, while HTML uses #bf0303 for its string class. This is a
syntax-theme/token-class difference, not the fill-shade exception or an antialiasing
excuse. Keep token differences outstanding for the syntax repair. Layout, incorrect
state, font/style metrics and renderer antialiasing are separate causes. There is no
blanket renderer exception; add one only for a demonstrated glyph region and hash.

Fixture commit identities must name actual Git objects rather than the illustrative
mockup commits. Footer wording uses captured facts. `C++ source code` is the recognized
MIME display name for the text fixture and is accepted as the expanded form of `C++`.
`PNG image` names the recognized type; `Binary` names its outside-app classification.
The valid pinned PNG is 18,637 bytes, or 18.2001953125 KiB; `18.20 KiB` and `18.2 KiB`
are truthful precision choices. These exceptions apply only to those values in the
identified header/footer regions and reference hashes. They grant no geometry, contrast,
encoding, line-ending, byte-size or general metadata waiver. Never edit fixture bytes
to manufacture a matching label. The historical incorrect conflict-count label remains
an observed defect, not an expected correctness contract or an exception.

At 1280×720 the measured viewports are 576 logical px high, exceeding eight 18 px rows.
The native replay exercises real selection, source choices, staging, Undo and Redo,
and binary action refusal. It retains the named controls and rendered-frame readiness.
Full window-fit/reachability and supplemental no-wrap journeys remain obligations at
the integrated wave gate. The owner's strict 960×540 control-fit/pane-height waiver
remains; scrolling and first-undecided-region visibility there are still required.

Every changed region remains `outstanding` in diagnostic reports, including regions
with a conditional documented exception. Exceptions never subtract changed pixels.
Later repairs must explain remaining differences and retain the approved originals,
profile, before/after captures, overlays, differences, close-ups and cleanup evidence.
