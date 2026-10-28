# UI design brief

This is the reference for how the Branch Truce window looks. [PRD.md](../PRD.md) says what users need and [TECH_SPEC.md](../TECH_SPEC.md) names the screens and components. This file sets the visual hierarchy, the layout, the shared components, and the Breeze icons and `Kirigami.Theme` colour roles each surface uses. Every icon name and colour role below was checked against kf6-breeze-icon-theme 6.24 and the Kirigami 6.24 headers on the development host.

The approved mockups in [ui-mockup/](ui-mockup/README.md) are the target:

| Mockup | State |
| --- | --- |
| [light-text.png](ui-mockup/light-text.png), [dark-text.png](ui-mockup/dark-text.png) | A text conflict in the three panes, with one conflict undecided, one changed passage, one accepted line and one ignored line |
| [light-binary.png](ui-mockup/light-binary.png), [dark-binary.png](ui-mockup/dark-binary.png) | A binary file selected; it has to be resolved outside the app |

Each PNG is rendered from the HTML file beside it at 1280x720. Pixel sizes quoted from the mockups below are CSS pixels in that HTML. They show what the `Kirigami.Units` values look like with Noto Sans 10 pt; the units are the rule. Section 10 lists the elements the app does not have yet and the one place where a kept rule differs from the mockups.

## 1. Visual hierarchy

### Quiet chrome, loud content

The content is the file list and the three code panes. Everything around them is chrome, and chrome stays quiet so the content reads first.

- The header band (the main toolbar) and the footer use `Kirigami.Theme.colorSet: Kirigami.Theme.Header`. The file toolbar uses the Window set. The file list and the panes use the View set, and the pane headers use the View set's `alternateBackgroundColor`. The connector handles between panes show the Window background.
- Panels are separated by 1 px hairlines, drawn with `Kirigami.Separator` so their colour comes from the theme. A hairline runs below the header band, above the footer, between the sidebar and the work area, below the list header and the file toolbar, below each pane header, and on both sides of each connector handle. Inside a toolbar a vertical `Kirigami.Separator` divides the action groups. The hairline is the one literal size in this brief.
- Chrome holds names, counts and short status phrases. Sentences belong in notices, placeholders and dialogs.
- Short state tags, such as a count beside a heading, use a `Pill` (section 4), not a framed button or bare text in brackets.

### Hierarchy through weight and muting

Weight and muting carry the hierarchy, not size or colour.

- Titles use `Kirigami.Heading` with `type: Kirigami.Heading.Type.Primary`, which is DemiBold. Titles inside bands (the repository name, "Conflicted files", the pane titles and the selected file name) use `level: 5`, the default font size. Placeholder titles keep the size `Kirigami.PlaceholderMessage` gives them.
- Secondary text uses `Kirigami.Theme.smallFont` in `Kirigami.Theme.disabledTextColor`: the operation beside the repository name, the directory beside a file name, a file row's status line, a pane's identity, and the footer.
- When one label pair names one thing, the important part carries the weight. The file toolbar shows `src/net/` muted and `retry.cpp` in DemiBold.
- A status line takes a status colour only when that colour is the fact: `positiveTextColor` for "Resolved", `neutralTextColor` for "Handle outside the app", `negativeTextColor` for the folded "Unresolved conflict" row. Other status text stays muted.
- No hand-set point sizes, italics or capitals for emphasis.

### Colours from the theme

Every colour comes from `Kirigami.Theme`, so it follows the colour scheme. Tints of theme colours are allowed: `Qt.alpha(role, a)`, `Kirigami.ColorUtils.tintWithAlpha` and `Kirigami.ColorUtils.linearInterpolation`. Hex values and `Qt.rgba` with literal channels are not. `Qt.darker` and `Qt.lighter` are not either, because they darken a dark scheme further instead of following it. Check every tint in BreezeLight and BreezeDark.

### Narrow windows

When the window narrows, chrome gives way in this order and the panes keep their width:

1. Toolbar labels collapse to icons. The action's `text` becomes the tooltip. Save and Mark resolved keep their labels longest.
2. Actions that still do not fit move into their toolbar's overflow menu, which `Kirigami.ActionToolBar` does on its own.
3. Low-priority status hides, first the footer's file facts, then the footer's progress bar, then the operation beside the repository name, then the directory beside each file name.

The outside-app count in the footer, the files-resolved count and the conflict count never hide (section 3).

Nothing in the chrome wraps. Every chrome label is one line and elides. Paths elide in the middle so the file name stays visible; other text elides at the end. An elided label shows its full text in a tooltip (`PlainLabel`, section 4).

## 2. Layout

The window is a `Kirigami.ApplicationWindow` holding one `Kirigami.Page`. The app launches straight into the session; there is no start page.

```
+------------------------------------------------------------------------------+
| [=] repository  operation      [^ (!) 1 conflict left v] | << Previous file  |
|                                   >> Next file | (i) Review handoff... | menu |
+--------------+---------------------------------------------------------------+
| Conflicted   | [icon] dir/name           Save  [Mark resolved...] | undo    |
| files    (3) |                                    redo | find | overflow     |
+--------------+---------------------------------------------------------------+
| file rows    | notices (only when there is something unusual to say)         |
|              +-------------------+-+-------------------+-+-------------------+
|              | left source       | | Result            | | right source      |
|              | (read-only)       |b| (editable)        |b| (read-only)       |
|              |                   | |                   | |                   |
+--------------+-------------------+-+-------------------+-+-------------------+
| 1 of 3 files resolved [===   ]  /!\ 1 file to handle outside the app  facts |
+------------------------------------------------------------------------------+
```

`b` is a connector handle. Every line in the diagram is a hairline.

### Window size

| Size | Width | Height | Mockup |
| --- | --- | --- | --- |
| Default | `Kirigami.Units.gridUnit * 70` | `Kirigami.Units.gridUnit * 40` | 1280x720 |
| Minimum | `Kirigami.Units.gridUnit * 48` | `Kirigami.Units.gridUnit * 28` | none |

The minimum must fit a 960x540 logical screen at the default font, because the window-fit journey checks that size (section 2, vertical budget).

### Header band

One `Kirigami.ActionToolBar` row in the Header colour set, from left to right:

1. **Sidebar toggle.** `sidebar-expand-left`, icon only, checkable and checked while the file list shows.
2. **Title.** The repository name as a `Kirigami.Heading`, then the operation in muted `smallFont`, for example "Merging feature/backoff into main". The name, not the path; the full path is in the tooltip.
3. **Conflict stepper.** One framed group: Previous conflict (`arrow-up`, icon only), the count with `vcs-conflicting` ("1 conflict left"), and Next conflict (`arrow-down`, icon only). It steps through the undecided conflicts of the selected file, and clicking the count moves to the next one. With none to step through it reads "No conflicts to step through" and its buttons are disabled. This is the undecided counter; it lives here and not in a pane header.
4. **Separator, then file navigation.** Previous file (`go-previous-skip`) and Next file (`go-next-skip`), with labels. They move to the previous and next unfinished file.
5. **Separator, then Review handoff…** (`documentinfo`, with label). It opens the handoff dialog.
6. **Application menu** (`application-menu`). It holds the rest of the navigation: previous and next difference, previous and next saved or reviewed file.

### Sidebar

A `SplitView` pane to the left of the work area, `Kirigami.Units.gridUnit * 14` wide by default (262 px in the mockup). It is not modal, can be resized, and collapses with the sidebar toggle. It uses the View colour set and lists the conflicted files and nothing else.

- **List header.** "Conflicted files" as a `Kirigami.Heading` and a neutral `Pill` with the number of files. It is as tall as the file toolbar, so their hairlines line up.
- **List.** `Kirigami.Units.smallSpacing` padding above the first row and below the last.
- **Rows.** Each row sits on a row plate (section 4). The leading icon is the file's status icon from section 5. The first line is the file name, then its directory in muted `smallFont`. The second line is one short status phrase in `smallFont`: "1 conflict left", "Resolved" in `positiveTextColor`, "Handle outside the app" in `neutralTextColor`. A row never carries a second status as a pill, and never joins statuses with dots or separators.

### File toolbar

A second `Kirigami.ActionToolBar` row for the selected file, in the Window colour set, from left to right:

1. **File.** The file's MIME-type icon (`text-x-c++src`, `image-png` and so on; `unknown` when there is none), then its path: the directory muted, the name in DemiBold.
2. **Terminal actions.** Save (`document-save`, flat, with label) and Mark resolved… (`dialog-ok-apply`, framed as the primary action, with label).
3. **Separator, then Undo and Redo** (`edit-undo`, `edit-redo`, icon only).
4. **Separator, then Find** (`edit-find`, icon only). It opens the search bar.
5. **Overflow menu** (`overflow-menu`). It holds Discard, "Discard draft and use staged result", Copy and Paste, the Source choices menu (whole-file and all non-conflicting choices), and the reading controls as checkable entries: Wrap lines, Show spaces and tabs, Show earlier version (`view-visible`) and Show conflict markers (`view-visible`).

The reading controls therefore sit in the file toolbar: Find as a button, the toggles in its overflow menu. Whole-file and "all non-conflicting" choices live in the Source choices menu, never as an always-visible row of buttons. Line and region choices live in the gutters.

Mark resolved is always available for a file the app can edit, as `PRD.md:103` requires. While source choices remain undecided it opens a confirmation that names how many; once none remain it acts at once. The ellipsis in its label is there because it may open that confirmation. When the selected file is outside the app, every file action is disabled, because there is no app result to save or resolve (the binary mockups).

### Notices

Notices sit between the file toolbar and the panes and take no height when there are none. Each is a `Kirigami.InlineMessage` for an unusual condition: a failed load or save, a recovery problem, a stale staged result, a mergetool wait, the parent's staging state, the result of a command, or the session-level outside-app warning described in section 3. One message per condition, one line unless the user expands it, with its action (retry, recheck) on the message.

### Panes

Three code panes in a `SplitView`: the two read-only sources around the editable result, with a connector handle between each source and the result. Each pane is a `PaneHeader` above its text. The title is the version's name and the identity says what it is, truthfully (`PRD.md:33`): "main", "Current branch · 6f2dd188f0"; "Result", "Editable"; "feature/backoff", "Incoming · 234b80a1fd". Commit ids use `fixedWidthFont`.

The earlier version (common ancestor) is not a fourth column. It opens on demand with Show earlier version, over or beside the panes, and closing it restores the editor state and selection (`PRD.md:37`).

An outside-app file shows a `Kirigami.PlaceholderMessage` in place of the panes, in the View background: the file's type icon, a title naming why it is outside the app ("Binary files are resolved outside Branch Truce"), and an explanation that names the next step (resolve it with another tool, then `git add` the path) and says the file stays in the list until Git reports it resolved.

### Pane spacing

| Measure | Value | Mockup |
| --- | --- | --- |
| Pane header height | Fixed, the same in all three panes: one line plus `Kirigami.Units.mediumSpacing` above and below | 30 px |
| Pane header side margins | `Kirigami.Units.largeSpacing` | 12 px |
| Text inset below the pane header | `Kirigami.Units.smallSpacing` | 6 px |
| Line-number column | Right-aligned in muted `smallFont`, as wide as the largest number, `Kirigami.Units.smallSpacing` after it | 34 px |
| Glyph column | `Kirigami.Units.iconSizes.small` wide, glyph centred | 16 px |
| Gap between glyph and code | `Kirigami.Units.largeSpacing` | 8 px |
| Gutter buttons | On the source pane's edge that faces the result, two `Kirigami.Units.iconSizes.small` buttons `Kirigami.Units.smallSpacing` apart | 46 px column |
| Inset from a split handle | `Kirigami.Units.smallSpacing` between the handle's hairline and the nearest text or button | none |
| Row height | `fixedWidthFont` line spacing plus `Kirigami.Units.smallSpacing` | 20 px for 12 px Hack |
| Connector handle | `Kirigami.Units.gridUnit` wide | 20 px plus two hairlines |

The vertical budget counts rows at this height. The pane's minimum height, the window-fit check and the helper that scrolls by line height all use the same row height; change them together.

### Connector bands

Each non-equal passage gets one band between a source pane and the result. The band sits in the connector handle, so it takes no extra row or column from the vertical budget.

- **Fill.** The band uses the passage's band tint from section 6: the role's text colour at about 40 % alpha, `positiveTextColor` for added or accepted, `activeTextColor` for changed, `negativeTextColor` for conflicting, `neutralTextColor` for hand-edited, and `disabledTextColor` at about 30 % for removed or ignored. Bands are stronger than the line fills so the eye can follow them across the handle.
- **Ends.** Each end of a band follows the top and bottom of its passage in that pane and moves with the pane's scroll. A band whose passage is off-screen is clipped, not repositioned.
- **Redraw.** Bands are redrawn whenever passages or decisions change, including after undo and redo.
- **Not a control.** A band only shows which passages correspond. Choices stay on the gutter buttons. A band never carries meaning by colour alone; the passage's gutter glyph (section 6) is the non-colour cue.

### Conflict markers

The result buffer keeps the Git marker lines (`<<<<<<<`, `|||||||`, `=======`, `>>>>>>>`) as text until the user decides them (`TECH_SPEC.md:90`, `TECH_SPEC.md:182`). This section covers only how the view presents them.

- The view folds each marker block into one row. The row carries the `!` glyph on `negativeBackgroundColor`, the label "Unresolved conflict" in `negativeTextColor` and DemiBold in the default font, and a small framed Take both button (`merge`). The line numbers skip the folded lines (11 then 18 in the mockup), so a conflict that still has markers stays visible and red marks only ranges that need a decision.
- Show conflict markers in the file toolbar's overflow menu shows the raw marker lines. Toggling changes presentation only; it never edits the buffer.
- Markers leave the buffer only when their section is fully decided, by a confirmed source choice that replaces the mapped range (`TECH_SPEC.md:90`). Folding, unfolding and scrolling never remove them.
- Reading aids never alter serialisation (`TECH_SPEC.md:182`). Save and Mark resolved write the buffer as it is, whether the markers are folded or shown.

### Footer

The footer status band (section 4) is one line in the Header colour set: the session's progress on the left, the outside-app count beside it, the selected file's facts on the right. It never wraps and never holds actions.

### Vertical budget

The window-fit journey in `tests/acceptance/JourneyAcceptanceTests.cpp` (its `checkFits` helper) pins these limits:

- At 1280x720 and at 960x540 every text pane is at least eight lines high, also with the source choices shown.
- Every named control is reachable: in view, after scrolling the page, or after opening the menu that holds it.
- Each pane header is one line high.
- At 1280x800 no file row wraps.

The chrome above and below the panes has a fixed allowance:

| Element | Height | Mockup |
| --- | --- | --- |
| Header band | one `Kirigami.ActionToolBar` row | 46 px |
| File toolbar and list header | one `Kirigami.ActionToolBar` row | 40 px |
| Notices | zero when empty; one line each otherwise | none shown |
| Search bar | zero while closed; one row while open | not shown |
| Pane header | one line, see pane spacing | 30 px |
| Footer | one line of `smallFont` with `Kirigami.Units.smallSpacing` above and below | 28 px |

In the mockup the bands take 144 px of 720, which leaves room for 28 rows in each pane. Nothing else may take permanent vertical space. A new control goes into an existing toolbar, a menu, a gutter or a dialog. At small window sizes the page may scroll rather than clip, as the journey allows at 960x540. If a layout change needs a different budget, update the journey and this section together.

## 3. One fact in one place

Each status fact has one surface. Before adding text, find the fact in this table.

| Fact | Surface | Form |
| --- | --- | --- |
| Repository | Header band title | Name in DemiBold; path in the tooltip |
| Operation and branches | Header band, beside the title | Muted phrase: "Merging feature/backoff into main" |
| A file's path | File row; the file toolbar for the selected file | Name, then muted directory |
| A file's state | File row status line | One phrase: "1 conflict left", "Resolved", "Handle outside the app" |
| Undecided conflicts in the selected file | Conflict stepper | Count as the stepper's label |
| Source identity | Pane header | Title, then muted role and short commit |
| What the result holds | Result pane header | Title "Result", identity such as "Editable" |
| Session progress | Footer | "1 of 3 files resolved" and a progress bar |
| Files outside the app | Footer | Warning icon and count in `neutralTextColor` |
| Encoding, line endings, type or size | Footer, right | Muted, "UTF-8 · LF · C++" or "Binary · 18.2 KiB" |
| Why the selected file is outside the app, and what to do | Placeholder in the work area | Title and one or two sentences |
| An unusual condition (failure, recovery, stale staging, mergetool wait) | Notice | One `InlineMessage`, action on it |
| Supported work complete, outside-app work remains | Full-width notice | `Warning` `InlineMessage` |
| A range's decision | Gutter | Glyph from section 6 |
| What blocks finishing | Handoff dialog | One line per blocker |

Rules:

- No sentence restates a notice. A label, placeholder or footer never repeats what a visible notice says, and two notices never say the same thing.
- A fact appears in a second place only as a control's own label or as the selected item's name. The stepper's "1 conflict left" repeats the selected row's status because it is the stepper's label; the file toolbar repeats the selected row's path because it names what the toolbar acts on.
- A status is a short phrase, not a sentence, and statuses are never joined with " · ". A middle dot only separates short facts of one kind in muted text: a pane's role and commit, the footer's file facts.

### Outside-app work

`PRD.md:21` requires unsupported files to say prominently, beside the file and in the session summary, that they need handling outside the app. `PRD.md:93` and `PRD.md:127` keep that warning visible after supported text work is complete.

- **Beside the file.** The row's status line reads "Handle outside the app" in `neutralTextColor`, with the `unknown` icon. Selecting the file shows the placeholder in the work area, which is the file-level notice.
- **In the session summary.** Whenever any file is outside the app, the footer shows `dialog-warning` and "1 file to handle outside the app" (or "N files"), both in `neutralTextColor`. It never hides for width.
- **Full-width warning.** A `Warning` `InlineMessage` appears only when every supported file is resolved and outside-app work remains. It says that supported text work is complete and how many files remain. It is not shown merely because an unsupported file exists.
- **Never counted as resolved.** Outside-app files count in the footer's total and never in its resolved count.

## 4. Components

### Shared components

| Component | Definition | Used by |
| --- | --- | --- |
| `PaneHeader` | Fixed-height row in `alternateBackgroundColor` with a hairline below. Title as `Kirigami.Heading` level 5, Primary. Optional one-line identity as a muted `smallFont` `PlainLabel`. Side margins and height from the pane spacing table. Its layout never sizes the row from its own children's widths, so the three headers align by construction. | Left, result and right panes; the earlier-version view |
| `Pill` | A short state tag: text, an optional icon name, and a tone (neutral, positive, negative, warning, highlight) mapped to `disabledTextColor`, `positiveTextColor`, `negativeTextColor`, `neutralTextColor` and `highlightColor`. Text in the tone colour and `smallFont`. Fill `Qt.alpha(tone, 0.14)`, 1 px border `Qt.alpha(tone, 0.4)`, radius half its height, `Kirigami.Units.smallSpacing` horizontal padding. One line. | List header count; the marker chip in the gutter; any other short state tag. Never a second status on a file row. |
| `PlainLabel` | A `Label` with `textFormat: Text.PlainText`, one line, eliding at the end (in the middle for paths). While elided it shows the full text in a tooltip after `Kirigami.Units.toolTipDelay`. | Every label that shows repository, path, branch, commit or status text: the header title and operation, file row name, directory and status, the file toolbar path, pane identities, footer text, handoff blockers, preview dialog fields |
| `PreviewDialog` | A `Kirigami.Dialog` for a change the user confirms after seeing it. A header with the path, a one-line summary and a detail line; a `BeforeAfterPanes` body; a confirm button whose text names its action and a Cancel button. Width, padding and entry spacing in `Kirigami.Units`. | Whole-file, all non-conflicting and replacement previews |
| `BeforeAfterPanes` | Two read-only panes side by side, each with a title, in `fixedWidthFont` with the code panes' tab stop. Takes an objectName prefix for its children. | Every before-and-after view in a `PreviewDialog` |
| `DiffStyle` | A QML singleton that maps each passage kind to its background role, text role, glyph, band tint and accessible description, from the tables in section 6, with one fallback for an unknown kind. | Gutter glyphs, line fills, connector bands, decision glyphs, previews |
| Row plate | An inset rounded rectangle behind a list row: `Kirigami.Units.smallSpacing` from the list's sides, radius `Kirigami.Units.cornerRadius`, `Kirigami.Units.smallSpacing` vertical and `Kirigami.Units.largeSpacing` horizontal padding. Selected: `highlightColor` with `highlightedTextColor`, secondary text at 0.8 alpha. Hover: a `Qt.alpha(hoverColor, 0.2)` tint. Keyboard focus: a 1 px `focusColor` outline. Never full-bleed. | Sidebar file rows; handoff dialog rows |
| Footer status | The one-line footer band: Header colour set, hairline above, `smallFont` in `disabledTextColor`. From left: progress text and a progress bar (`Kirigami.Units.gridUnit * 5` wide, `Kirigami.Units.smallSpacing` high, rounded, `highlightColor` over `Qt.alpha(disabledTextColor, 0.25)`), then the outside-app count, then a spacer, then the selected file's facts. No actions. | The session footer |

### Kirigami components per surface

| Surface | Component |
| --- | --- |
| File row | `SubtitleDelegate` from `org.kde.kirigami.delegates`, on a row plate |
| Notices | `Kirigami.InlineMessage` with a `Kirigami.MessageType` |
| Empty, unavailable and outside-app states | `Kirigami.PlaceholderMessage` |
| Dialogs | `Kirigami.Dialog`, or `Kirigami.PromptDialog` for confirm and cancel; `PreviewDialog` for previews |
| Titles | `Kirigami.Heading` |
| Toolbars | `Kirigami.ActionToolBar` with `Kirigami.Action` items |
| Hairlines | `Kirigami.Separator` |

**Notices.** One `Kirigami.InlineMessage` per condition, with the matching type: `Information` for context, `Positive` for a completed step, `Warning` for stale staging or for outside-app work that remains once supported work is complete, `Error` for a failed load or save.

**Empty states.** Empty content, unavailable earlier context and content that cannot be opened are three different `Kirigami.PlaceholderMessage` texts (`PRD.md:71`). A failed load never shows as an empty result.

**Dialogs.** Handoff, replacement, whole-file and non-conflicting choices open as dialogs. Each states what will change and names the files it affects. Destructive buttons say what they do ("Discard unsaved edits"), not "OK".

## 5. Icons

All names are Breeze icon names, used through `Kirigami.Icon` or an action's `icon.name`. Do not ship custom icons for these roles.

| Meaning | Icon name |
| --- | --- |
| Toggle sidebar | `sidebar-expand-left` |
| Previous / next conflict (stepper) | `arrow-up` / `arrow-down` |
| Previous / next file (unfinished) | `go-previous-skip` / `go-next-skip` |
| Previous / next difference | `go-previous` / `go-next` |
| Previous / next saved or reviewed file | `go-up` / `go-down` |
| Review handoff | `documentinfo` |
| Application menu | `application-menu` |
| Toolbar overflow | `overflow-menu` |
| Save | `document-save` |
| Mark resolved | `dialog-ok-apply` |
| Discard | `document-revert` |
| Undo / Redo | `edit-undo` / `edit-redo` |
| Copy / Paste | `edit-copy` / `edit-paste` |
| Search | `edit-find` |
| Accept from the left / right side | `arrow-right` / `arrow-left` |
| Ignore a contribution | `dialog-cancel` or `edit-delete-remove` |
| Take both sides | `merge` |
| Show earlier version | `view-visible` |
| Show conflict markers | `view-visible` |
| Selected file's type | the file's MIME-type icon, `unknown` when none |
| File: conflicting | `vcs-conflicting` |
| File: untouched or no conflict left | `vcs-normal` |
| File: edited by hand or in progress | `vcs-locally-modified` |
| File: resolved and saved | `checkmark` |
| File: outside the app | `unknown` |
| Notice or count: warning / error | `dialog-warning` / `dialog-error` |
| Notice: data at risk | `data-warning` |

The accept arrow points from the source towards the result: `arrow-right` in the left pane, `arrow-left` in the right pane. Use `edit-delete-remove` for Ignore beside a gutter arrow, as the mockups do, and `dialog-cancel` where Ignore is a labelled button. Use one of the two per surface.

An icon alone never carries meaning. Icon-only buttons set `text` (shown as a tooltip or in the overflow menu) so the name is available to the user and to tests.

## 6. Colour roles

| Meaning | Line fill | Text | Band tint |
| --- | --- | --- | --- |
| Added | `positiveBackgroundColor` | `positiveTextColor` | `positiveTextColor` at 0.4 |
| Changed (modified) | `activeBackgroundColor` | `activeTextColor` | `activeTextColor` at 0.4 |
| Removed | none; `disabledTextColor` on the text | `disabledTextColor` | `disabledTextColor` at 0.3 |
| Conflicting | `negativeBackgroundColor` | `negativeTextColor` | `negativeTextColor` at 0.4 |
| Hand-edited | `neutralBackgroundColor` | `neutralTextColor` | `neutralTextColor` at 0.4 |
| Accepted side | `positiveBackgroundColor` | `positiveTextColor` | `positiveTextColor` at 0.4 |
| Ignored side | none; `disabledTextColor` on the text | `disabledTextColor` | `disabledTextColor` at 0.3 |
| Conflict markers remain | `negativeBackgroundColor` | `negativeTextColor` | `negativeTextColor` at 0.4 |
| Selected row | `highlightColor` | `highlightedTextColor` | none |

Band tints are `Qt.alpha(role, a)` with the alpha shown. A removed or ignored range gets no background fill, only `disabledTextColor` on its text. The negative roles are reserved for conflicts, so red marks only ranges that need a decision. Outside the panes, `neutralTextColor` is the warning colour for outside-app work.

Colour never carries meaning alone (`PRD.md:35`, `PRD.md:73`). Every coloured range also has a glyph in the gutter's glyph column, drawn bold in the range's text colour:

| Range | Glyph |
| --- | --- |
| Added | `+` |
| Removed | `−` |
| Changed | `~` |
| Conflicting | `!` |
| Hand-edited | `m` |
| Accepted side | `✓` |
| Ignored side | `×` |
| Conflict markers remain | `!` |

**Gutter buttons.** On a region's first line, each source pane shows Accept and Ignore on its edge that faces the result: Ignore then Accept in the left pane, Accept then Ignore in the right pane. They are small framed buttons in the View background. The Accept icon is drawn in `activeTextColor` and the Ignore icon in `negativeTextColor`.

**Decided regions.** When a region is decided, its gutter buttons are replaced by the decision glyph: `✓` for the accepted side, `×` for the ignored side. Undo is the way back (`PRD.md:43`); the buttons return when the decision is undone. The ignored side keeps its text, only muted, so the user can still read what was declined.

Within a changed line, word-level highlights use the same role as the line at a stronger weight, and carry no separate control (`PRD.md:35`).

## 7. Typography and sizing

- Code in all three panes and in `BeforeAfterPanes` uses `Kirigami.Theme.fixedWidthFont`. Commit ids in pane headers use it too.
- Everything else uses the theme's default font, or `Kirigami.Theme.smallFont` for secondary text (section 1). Titles use `Kirigami.Heading`, not hand-set point sizes.
- Spacing, margins, radii, icon sizes and window sizes use `Kirigami.Units` (`smallSpacing`, `mediumSpacing`, `largeSpacing`, `gridUnit`, `cornerRadius`, `iconSizes.*`). The only literal size is the 1 px hairline and the 1 px outlines drawn with it (`Pill` border, focus outline).
- Focus is visible on every control; use the style's own focus ring and do not remove it.

## 8. Competitor patterns adopted

| Pattern | Source |
| --- | --- |
| Gutter arrow and X per side; "apply all non-conflicting" | [JetBrains: resolve conflicts](https://www.jetbrains.com/help/idea/resolve-conflicts.html) |
| Connector graphic between a source pane and the result; conflict markers hidden from the merged text | [JetBrains: resolve conflicts](https://www.jetbrains.com/help/idea/resolve-conflicts.html) |
| Conflict counter that doubles as next-conflict | [VS Code: merge conflicts](https://code.visualstudio.com/docs/sourcecontrol/merge-conflicts) |
| Order-aware take-both; Mark resolved tied to the number of conflicts remaining | [KDiff3: merging](https://docs.kde.org/stable_kf6/en/kdiff3/kdiff3/merging.html) |
| Two terminal actions only | [Sublime Merge: getting started](https://www.sublimemerge.com/docs/getting_started) |
| Status colours; do not rely on colour alone | [KDE HIG: status changes](https://develop.kde.org/hig/status_changes/) |
| Icon choice and use | [KDE HIG: icons](https://develop.kde.org/hig/icons/) |
| Layout and navigation | [KDE HIG: layout and navigation](https://develop.kde.org/hig/layout_and_nav/) |
| Displaying content | [KDE HIG: displaying content](https://develop.kde.org/hig/displaying_content/) |

How they apply here:

- The per-side arrow in the gutter is the accept action for a range; the ignore icon beside it declines that side. Both are reversible with undo.
- The conflict stepper in the header band shows the undecided conflicts left in the selected file and, when clicked, moves to the next one.
- Take both respects the order of the sources and never reorders text silently.
- Mark resolved is always available for a file the app can edit (`PRD.md:103`). While source choices remain undecided it opens a confirmation that names how many, instead of being disabled. Once none remain, it acts at once.
- The terminal actions are Save (keep an app-only draft) and Mark resolved (write the result and resolve the file in Git). Nothing else ends work on a file.

## 9. Test contract

The acceptance and UI tests find controls by `objectName` and read their state by property. A redesign must keep that working.

- Controls stay `QQuickItem`s carrying their existing `objectName`s. Changing a component type is fine; renaming or dropping an `objectName` is not. A control that moves into a menu keeps its `objectName` there.
- Icon-only buttons set `text`. Tests and assistive tools read the label there, not from the icon.
- Status text stays in `Label`s, so tests can read `text` directly. Do not move it into painted items or images.
- A change that alters when a control is visible, or where it lives, updates the journeys that check it in the same change.
- The size claims in section 2 are enforced by the window-fit journey in `tests/acceptance/JourneyAcceptanceTests.cpp`. If a layout change needs a different budget, update the journey and section 2 together.
- `branch_truce_ui_qmllint` must stay at zero warnings.
- A change under `src/ui` ships with before and after captures of every surface it affects, in BreezeLight and BreezeDark, attached to the resolution of its work item. Tests and `qmllint` do not show the whole window. `tools/preview-app.sh` takes the captures; set `PREVIEW_FILE` when the change shows on a text or Markdown file (see [build.md](build.md)).

## 10. Targets and differences

The mockups show elements the app does not have yet. They are targets of this brief:

- the progress bar and the file facts in the footer;
- the accept and ignore buttons in the source pane gutters;
- Take both on the folded conflict row in the result.

The rest of the layout above (the header band with the repository name and conflict stepper, the row plates, the pane headers, the footer status and the shared components) is also the target where the app still differs from it.

One kept rule differs from the mockups. The mockups approximate line fills by mixing a role's text colour into the View background, 14 % in Breeze Light and 24 % in Breeze Dark. The app uses the background roles in section 6 (`positiveBackgroundColor` and the others), which the colour scheme derives, so its fills can differ slightly in shade from the mockups. The roles win.
