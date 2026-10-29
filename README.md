<h1>
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/branding/svg/for-dark/branch-truce-horizontal.svg">
    <img src="assets/branding/svg/for-light/branch-truce-horizontal.svg" alt="Branch Truce" width="360">
  </picture>
</h1>

A standalone text conflict resolver for Git on Linux and KDE. Compare both branches, compose your result and get back to your merge or rebase. Works offline, with no account.

**[Download](https://github.com/e0ipso/branch-truce/releases/latest)** · **[Get started](#use-it)** · **[Set up git mergetool](docs/mergetool.md#register-the-tool)**

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/comparison-dark.png">
  <img src="docs/images/comparison-light.png" alt="A four-file merge in Branch Truce: competing C++ retry policies, status checks, backoff and logging changes, with both branches beside the editable result" width="1200">
</picture>

*One merge, four conflicted files. Both branches stay visible while you build the result in the middle. Follows your desktop's light or dark colour scheme.*

## See it in action

<img src="docs/images/demo.gif" alt="Resolving a C++ merge: accept a change, undo and redo it, compare the common earlier version, then save a draft" width="1200">

*Choose a change, see the result, undo it and try again. Open the common earlier version whenever you need context. [Watch the video](docs/images/demo.mp4).*

## Work through the conflict

<table>
  <tr>
    <th>Review the exact change</th>
    <th>Get the earlier context</th>
  </tr>
  <tr>
    <td><a href="docs/images/choice-preview.png"><img src="docs/images/choice-preview.png" alt="Before and after preview of taking the logging changes from both branches" width="580"></a></td>
    <td><a href="docs/images/earlier-version.png"><img src="docs/images/earlier-version.png" alt="The common earlier version showing the original retry settings" width="580"></a></td>
  </tr>
  <tr>
    <td>Accept, ignore or take both sides in either order. Preview broader changes before applying them, then edit the result yourself.</td>
    <td>See what both branches started from. Syntax highlighting, word-level differences and connected passages help you follow the changes.</td>
  </tr>
</table>

Step through remaining conflicts, find a passage, wrap long lines or reveal spaces and tabs. Choices and typing share an undo history.

Your work survives closing the window or a crash. Branch Truce restores edits and choices when you reopen the same merge or rebase. Files it cannot edit stay in the file list with a reason, so you can handle them in another tool.

## Install

Download the `.deb` for your system from the [latest release](https://github.com/e0ipso/branch-truce/releases/latest).

| System | Package |
| --- | --- |
| Kubuntu 26.04 | `branch-truce_<version>_amd64-ubuntu26.04.deb` |
| Tuxedo OS 24.04 | `branch-truce_<version>_amd64-tuxedo24.04.deb` |

Install the downloaded package. Apt also installs the dependencies:

```sh
sudo apt install ./branch-truce_*_amd64-ubuntu26.04.deb
# On Tuxedo OS, use the downloaded *_amd64-tuxedo24.04.deb instead.
```

Packages have run in containers of both systems. Real desktop validation is still pending. See [platform validation](docs/platform-validation.md) for details, or [build from source](docs/build.md).

## Use it

When Git stops a merge or rebase on conflicts, open the repository:

```sh
branch-truce                     # current repository
branch-truce /path/to/repository
```

1. Pick a file and compare the changes. Opening a session leaves your files and Git index untouched.
2. Compose your result. **Save** keeps a draft inside Branch Truce. **Mark resolved** writes the reviewed result to the file and stages that path in Git.
3. Return to your terminal to continue the merge or rebase. Branch Truce leaves continuing and committing to you.

Prefer `git mergetool`? [Register Branch Truce](docs/mergetool.md#register-the-tool), then run:

```sh
git mergetool --tool=truce
```

## Contributing

See [build and test instructions](docs/build.md) to get started. The [knowledge base](.ai/kenkeep/ENTRY.md) records project conventions; [PRD.md](PRD.md) and [TECH_SPEC.md](TECH_SPEC.md) describe scope and architecture.

## License

[GPL-3.0-or-later](LICENSE).
