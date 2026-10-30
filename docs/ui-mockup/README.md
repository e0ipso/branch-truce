# Window mockups

The target look of the Branch Truce window. [ui-design.md](../ui-design.md) states the rules behind it.

| Files | State |
| --- | --- |
| `light-text.png`, `dark-text.png` | A text conflict in the three comparison panes, with one conflict undecided and one decided |
| `light-binary.png`, `dark-binary.png` | A binary file selected; it has to be resolved outside the app |

The sample repository and code also have a maintained [native fixture and replay](../../tools/native-replay/README.md). Its pinned sources preserve the approved bytes independently of changes to this HTML. Each PNG is the HTML file beside it rendered as a 1280x720 window at 1.5x scale. The HTML inlines the Breeze icons and uses the Breeze Light and Breeze Dark colour values with Noto Sans and Hack, so it looks right on a machine with those fonts. Each page takes its favicon from `../../assets/branding/web/`, so open the pages from a repository checkout. To render one again:

```sh
chrome --headless --hide-scrollbars --window-size=1280,720 --force-device-scale-factor=1.5 \
    --screenshot=light-text.png light-text.html
```

The app does not have these elements yet: the progress bar and file facts in the footer, Take both on the folded conflict row, and the accept and ignore buttons in the gutters.
