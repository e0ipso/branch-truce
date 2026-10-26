# UI design brief

This is the reference for the KDE-native look of the Branch Truce window. It describes the target, not the steps to reach it. [PRD.md](../PRD.md) says what users need; [TECH_SPEC.md](../TECH_SPEC.md) names the screens and components. This file says how those surfaces are laid out and which Kirigami components, Breeze icons and `Kirigami.Theme` colour roles each one uses. Every icon name and colour role below was checked against kf6-breeze-icon-theme 6.24 and the Kirigami 6.24 headers on the development host.

The starting point was a window with 26 buttons and about 30 text blocks visible at once, because each surface had been designed alone. This brief fixes that: the code panes get most of the space, and every other control sits in a toolbar, menu, gutter or dialog.

## 1. Layout

The window is a `Kirigami.ApplicationWindow` holding one `Kirigami.Page`. The app launches straight into the session; there is no start page.

```
+--------------------------------------------------------------+
| main toolbar: navigation + session actions   [sidebar toggle]|
+--------------+-----------------------------------------------+
| sidebar      | file toolbar: Save, Mark resolved, Discard,   |
| (file list)  |   Undo, Redo, Copy, Paste, Find               |
|              +-----------------------------------------------+
|              | inline notices (only when something to say)   |
|              +--------------+----------------+---------------+
|              | left source  | result         | right source  |
|              | (read-only)  | (editable)     | (read-only)   |
+--------------+--------------+----------------+---------------+
| footer: one line of status                                   |
+--------------------------------------------------------------+
```

- **Sidebar.** A `SplitView` pane to the left of the work area. It is not modal, can be resized, and collapses with an action using `sidebar-expand-left`. It lists the conflicted files and nothing else.
- **Main toolbar.** A `Kirigami.ActionToolBar` for movement between differences and unfinished files, and for session actions such as opening the handoff dialog. Actions that do not fit move into the overflow menu automatically.
- **File toolbar.** A second `Kirigami.ActionToolBar` for actions on the selected file: save, mark resolved, discard, undo, redo, copy, paste, find.
- **Panes.** Three code panes in a `SplitView`: the two read-only sources around the editable result. Each pane has a header naming its version truthfully (PRD.md:33). The earlier version (common ancestor) is not a fourth column. It opens on demand with the `view-visible` action, over or beside the panes, and closing it restores the editor state and selection (PRD.md:37).
- **Footer.** One line: the selected file's state and the session's progress. It never wraps and never holds actions.
- **Choice controls.** Whole-file and "all non-conflicting" choices live in menus on the toolbars, not as always-visible rows of buttons. Line and region choices live in the gutters.

### Vertical budget

At 1280x720 the three panes keep at least eight visible lines of text, with the source choices shown. The acceptance test at `tests/acceptance/JourneyAcceptanceTests.cpp:1383-1437` pins this, and also checks 960x540 and 1280x800. The chrome above and below the panes therefore has a fixed allowance:

| Element | Height |
| --- | --- |
| Main toolbar | one `Kirigami.ActionToolBar` row |
| File toolbar | one `Kirigami.ActionToolBar` row |
| Pane headers | one line each, title plus one-line identity |
| Footer | one line |
| Inline messages | zero when empty; one message at a time otherwise, collapsed to a single line unless the user expands it |

Nothing else may take permanent vertical space. A new control goes into an existing toolbar, a menu, a gutter or a dialog. At small window sizes the page may scroll rather than clip, as the test already allows at 960x540.

## 2. Components per surface

| Surface | Component |
| --- | --- |
| File row | `SubtitleDelegate` from `org.kde.kirigami.delegates` |
| Notices | `Kirigami.InlineMessage` with a `Kirigami.MessageType` |
| Empty and unavailable states | `Kirigami.PlaceholderMessage` |
| Dialogs | `Kirigami.Dialog`, or `Kirigami.PromptDialog` for confirm/cancel |
| Section titles | `Kirigami.Heading` |
| Toolbars | `Kirigami.ActionToolBar` with `Kirigami.Action` items |

**File rows.** Each row has a leading status icon, the path as title, and a one-line subtitle with the reason or progress (for example "2 of 5 differences decided"). The current file is bold. Status is shown as an icon plus text, never as a sentence joined with dots or separators. A file that cannot be edited shows its reason in the subtitle and the `unknown` icon.

**Notices.** One `Kirigami.InlineMessage` per condition, with the matching type: `Information` for context, `Positive` for a completed step, `Warning` for stale staging or unsupported files that remain, `Error` for a failed load or save. A notice that has an action (retry, recheck) carries it as an action on the message, not as a separate button elsewhere.

**Empty states.** Empty content, unavailable earlier context and content that cannot be opened are three different `Kirigami.PlaceholderMessage` texts (PRD.md:71). A failed load never shows as an empty result.

**Dialogs.** Handoff, replacement, whole-file and non-conflicting choices open as dialogs. Each states what will change and names the files it affects. Destructive buttons say what they do ("Discard unsaved edits"), not "OK".

## 3. Icons

All names are Breeze icon names, used through `Kirigami.Icon` or an action's `icon.name`. Do not ship custom icons for these roles.

| Meaning | Icon name |
| --- | --- |
| Save | `document-save` |
| Mark resolved | `dialog-ok-apply` |
| Discard | `document-revert` |
| Undo / Redo | `edit-undo` / `edit-redo` |
| Copy / Paste | `edit-copy` / `edit-paste` |
| Previous / next difference | `go-previous` / `go-next` |
| Previous / next unfinished file | `go-previous-skip` / `go-next-skip` |
| Accept from the left / right side | `arrow-left` / `arrow-right` |
| Ignore a contribution | `dialog-cancel` or `edit-delete-remove` |
| Take both sides | `merge` |
| Search | `edit-find` |
| Show earlier version | `view-visible` |
| Toggle sidebar | `sidebar-expand-left` |
| File: conflicting | `vcs-conflicting` |
| File: untouched or no conflict left | `vcs-normal` |
| File: edited by hand or in progress | `vcs-locally-modified` |
| File: resolved and saved | `checkmark` |
| File: unsupported | `unknown` |
| Notice: warning / error | `dialog-warning` / `dialog-error` |
| Notice: data at risk | `data-warning` |

Use `dialog-cancel` for Ignore where a labelled button is shown and `edit-delete-remove` where it sits beside a gutter arrow. Use one of the two per surface.

An icon alone never carries meaning. Icon-only buttons set `text` (shown as a tooltip or in the overflow menu) so the name is available to the user and to tests.

## 4. Colour roles

Colour comes from `Kirigami.Theme` only. No hex values, no `Qt.rgba` tints, no `Qt.darker` or `Qt.lighter` on theme colours.

| Meaning | Background | Text |
| --- | --- | --- |
| Added | `positiveBackgroundColor` | `positiveTextColor` |
| Changed (modified) | `activeBackgroundColor` | `activeTextColor` |
| Removed | `disabledTextColor` tint on the text, no background fill | `disabledTextColor` |
| Conflicting | `negativeBackgroundColor` | `negativeTextColor` |
| Hand-edited | `neutralBackgroundColor` | `neutralTextColor` |
| Selected row | `highlightColor` | `highlightedTextColor` |

A removed range gets only a `disabledTextColor` tint. The negative roles are reserved for conflicts, so red marks only ranges that need a decision.

Colour never carries meaning alone (PRD.md:35, PRD.md:73). Every coloured range also has a gutter glyph:

| Range | Glyph |
| --- | --- |
| Added | `+` |
| Removed | `−` |
| Changed | `~` |
| Conflicting | `!` |
| Hand-edited | `m` |

Within a changed line, word-level highlights use the same role as the line at a stronger weight, and carry no separate control (PRD.md:35).

## 5. Typography and sizing

- Code in all three panes uses `Kirigami.Theme.fixedWidthFont`.
- Everything else uses the default font from the theme. Titles use `Kirigami.Heading` levels, not hand-set point sizes.
- All spacing, margins, icon sizes and minimum sizes use `Kirigami.Units` (`smallSpacing`, `largeSpacing`, `gridUnit`, `iconSizes.*`). There are no literal pixel values.
- Focus is visible on every control; use the style's own focus ring and do not remove it.

## 6. Competitor patterns adopted

| Pattern | Source |
| --- | --- |
| Gutter arrow and X per side; "apply all non-conflicting" | [JetBrains: resolve conflicts](https://www.jetbrains.com/help/idea/resolve-conflicts.html) |
| Conflict counter that doubles as next-conflict | [VS Code: merge conflicts](https://code.visualstudio.com/docs/sourcecontrol/merge-conflicts) |
| Order-aware take-both; Mark resolved tied to the number of conflicts remaining | [KDiff3: merging](https://docs.kde.org/stable_kf6/en/kdiff3/kdiff3/merging.html) |
| Two terminal actions only | [Sublime Merge: getting started](https://www.sublimemerge.com/docs/getting_started) |
| Status colours; do not rely on colour alone | [KDE HIG: status changes](https://develop.kde.org/hig/status_changes/) |
| Icon choice and use | [KDE HIG: icons](https://develop.kde.org/hig/icons/) |
| Layout and navigation | [KDE HIG: layout and navigation](https://develop.kde.org/hig/layout_and_nav/) |
| Displaying content | [KDE HIG: displaying content](https://develop.kde.org/hig/displaying_content/) |

How they apply here:

- The per-side arrow in the gutter is the accept action for a range; the ignore icon beside it declines that side. Both are reversible with undo.
- The difference counter in the main toolbar shows remaining decisions and, when clicked, moves to the next one.
- "Take both" respects the order of the sources and never reorders text silently.
- Mark resolved is always available, as PRD.md:103 requires. While source choices remain undecided it opens a confirmation that names how many, instead of being disabled. Once none remain, it acts at once.
- The terminal actions are Save (keep an app-only draft) and Mark resolved (write the result and resolve the file in Git). Nothing else ends work on a file.

## 7. Test contract

The acceptance and UI tests find controls by `objectName` and read their state by property. A redesign must keep that working.

- Controls stay `QQuickItem`s carrying their existing `objectName`s. Changing a component type is fine; renaming or dropping an `objectName` is not.
- Icon-only buttons set `text`. Tests and assistive tools read the label there, not from the icon.
- Status text stays in `Label`s, so tests can read `text` directly. Do not move it into painted items or images.
- Size claims in this brief are enforced by `tests/acceptance/JourneyAcceptanceTests.cpp:1383-1437`. If a layout change needs a different budget, update the test and section 1 together.
- `branch_truce_ui_qmllint` must stay at zero warnings.
