# Window mockups

The target look of the Branch Truce window. [ui-design.md](../ui-design.md) states the rules behind it.

| Files | State |
| --- | --- |
| `light-text.png`, `dark-text.png` | A text conflict in the three comparison panes, with one conflict undecided and one decided |
| `light-binary.png`, `dark-binary.png` | A binary file selected; it has to be resolved outside the app |
| `light-ready.png`, `dark-ready.png` | The text conflict with the Open repository… button and its separator in the header |
| `light-error.png`, `dark-error.png` | The window opened on a folder that is not a repository: the error placeholder with its Open repository… button |

The text and binary pages are the references that [native replay](../../tools/native-replay/README.md) pins by hash, so they keep the header they were approved with. The ready pages show the same text conflict with the current header. The native replay also keeps the sample repository and code, and its pinned sources preserve the approved bytes independently of changes to this HTML.

Each PNG is the HTML file beside it rendered as a 1280x720 window at 1.5x scale. The HTML inlines the Breeze icons and uses the Breeze Light and Breeze Dark colour values with Noto Sans and Hack, so it looks right on a machine with those fonts. Each page takes its favicon from `../../assets/branding/web/`, so open the pages from a repository checkout. To render one again:

```sh
chrome --headless --hide-scrollbars --window-size=1280,720 --force-device-scale-factor=1.5 \
    --screenshot=light-text.png light-text.html
```

[ui-design.md](../ui-design.md#10-targets-and-differences) lists the reference
elements the native app implements and the kept rule that differs from the
mockups. The approved files remain reference artifacts. Native replay retains
independent captures and measured residuals and does not claim that every
reference pixel matches.
