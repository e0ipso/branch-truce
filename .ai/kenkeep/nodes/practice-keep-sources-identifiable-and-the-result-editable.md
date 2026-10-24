---
type: practice
title: Keep sources identifiable and the result editable
description: >-
  Require truthful source identities, an editable center result, earlier
  context, and reading aids that preserve text.
tags:
  - product
  - comparison
  - editing
kk_schema_version: 3
kk_id: practice-keep-sources-identifiable-and-the-result-editable
kk_derived_from:
  - PRD.md
  - src/ui/ComparisonView.qml
  - src/ui/Main.qml
  - src/app/ApplicationController.cpp
  - docs/editor-integration.md
kk_relates_to: []
kk_depends_on: []
kk_confidence: medium
---
Show two read-only source versions around an editable result in the center. Label all three clearly and identify each source truthfully for the current merge or rebase. Use meaningful names and version descriptions when branch names are insufficient. Make uncertain identity visible; do not rely only on position or labels such as 'mine'.

Preserve the repository file's existing content when opening a result, including manual edits and previously combined changes. Do not silently regenerate it from an earlier source. If a choice cannot be attached safely, explain the limitation and keep direct editing available for otherwise supported text.

Let the user compare the result with either original source throughout review. Provide the common earlier version on demand when available, and explain when it is unavailable. Let the user show and hide it without losing the result or choices. Do not consume permanent reading space with it unless the user chooses to view it.

Show corresponding passages with enough surrounding text to judge them. Distinguish additions, removals, modifications, and competing changes using more than color. Use word-level highlighting without suggesting a separate acceptance control for every highlighted word. Keep original context accessible when correspondence is uncertain.

Provide search within sources and result with a clear scope, line wrapping, and controls to reveal spaces and tabs. Keep whitespace-only changes discoverable. Make these reading controls preserve the text and resolution decisions.

[ComparisonView](../../../src/ui/ComparisonView.qml) shows resizable labeled source panes around an editable center result, and shows common earlier context when that capture exists. [Main](../../../src/ui/Main.qml) hides and shows the source-choice list. Hiding that list leaves the comparison panes in place. [ApplicationController](../../../src/app/ApplicationController.cpp) owns direct result edits and the basic undo, redo, and clipboard shortcuts. Its event filter groups the document notifications from one native input event, including an input-method commit, into one command. The result `TextArea` sets `TextArea.flickable` on its `Flickable`, and that scroll surface follows the editor content. Search, space and tab markers, and full keyboard operation across files, choices, review, and saving are absent from this comparison. Physical desktop checks of composed input, clipboard, pointer selection, scrolling, and theme behavior have not been run. See [editor integration](../../../docs/editor-integration.md).

<!-- kk:citations:start -->
# Citations

[1] [PRD.md](../../../PRD.md)
[2] [src/ui/ComparisonView.qml](../../../src/ui/ComparisonView.qml)
[3] [src/ui/Main.qml](../../../src/ui/Main.qml)
[4] [src/app/ApplicationController.cpp](../../../src/app/ApplicationController.cpp)
[5] [docs/editor-integration.md](../../../docs/editor-integration.md)
<!-- kk:citations:end -->
