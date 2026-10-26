---
type: practice
title: Follow the UI design brief for the KDE-native window
description: >-
  docs/ui-design.md sets layout, actions and colour roles; panes get the space
  and red marks only ranges that need a decision.
tags:
  - ui
  - design
  - kde
  - kirigami
  - qml
kk_schema_version: 3
kk_id: practice-follow-the-ui-design-brief-for-the-kde-native-window
kk_derived_from: []
kk_relates_to:
  - practice-design-desktop-apps-around-screens-and-components
  - practice-separate-save-from-mark-resolved
  - map-startup-style-colour-scheme-and-icon-theme
kk_depends_on: []
kk_confidence: high
---
[docs/ui-design.md](../../../docs/ui-design.md) is the reference for how the Branch Truce window looks and is laid out: which Kirigami components hold each surface, the toolbar actions and their icons, and the colour roles. [PRD.md](../../../PRD.md) still says what users need and [TECH_SPEC.md](../../../TECH_SPEC.md) names the screens and components.

The code panes get most of the window. Every other control sits in a toolbar, menu, gutter or dialog rather than as another visible button or text block. Designing each surface alone fills the window with controls, so place a new control within that layout.

Mark resolved is always available, as PRD.md requires. While source choices remain undecided it opens a confirmation that names how many instead of being disabled; once none remain it acts at once. Save and Mark resolved are the only actions that end work on a file.

Colour roles follow Kirigami's theme: a removed range gets only a `disabledTextColor` tint with no background, and the negative roles (`negativeBackgroundColor`, `negativeTextColor`) are reserved for conflicts, so red marks only ranges that need a decision.

<!-- kk:related:start -->
# Related

- Related: [practice-design-desktop-apps-around-screens-and-components](practice-design-desktop-apps-around-screens-and-components.md)
- Related: [practice-separate-save-from-mark-resolved](practice-separate-save-from-mark-resolved.md)
- Related: [map-startup-style-colour-scheme-and-icon-theme](map-startup-style-colour-scheme-and-icon-theme.md)
<!-- kk:related:end -->
