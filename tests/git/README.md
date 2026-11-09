# Real Git fixtures

One suite per unit under test, each its own CTest entry:

| File | CTest entry | Covers |
|---|---|---|
| `GitAdapterTests.cpp` | `git.adapter` | discovery, literal paths, helper and offline policy, staging, failure, cancellation and limits |
| `ConflictClassifierTests.cpp` | `git.conflict-classification` | which conflicts are editable and why the others are not |
| `TextConversionTests.cpp` | `git.text-conversion` | decoding sources, preparing and verifying results, per text format |
| `UserConfigIsolationTests.cpp` | `git.user-config-isolation` | whether the adapter's Git reads the account's or the system's configuration |
| `MergetoolParentTests.cpp` | `git.mergetool-parent` | a real `git mergetool` parent against the launcher |

`GitFixtures.h` holds what the first four share. `ParentStagingFixture.cpp` is
the tool `git mergetool` runs in the adapter's parent staging cases.

The suites run the fixture Git that `tests/fixtures/FixtureGit.h` selects:
`BRANCH_TRUCE_TEST_GIT` when set to an absolute path, else the first `git` on
`PATH`, else `/usr/bin/git`. `git.adapter` prints the selected executable and
version and checks the Git 2.43 compatibility floor. The adapter was exercised
with Git 2.43.0; no additional Git version was exercised.

Run from the checkout root:

```sh
git --version
./tools/prepare-worktree.sh desktop
cmake --build build/desktop --parallel 2
ctest --test-dir build/desktop --output-on-failure -V -R '^(git\.|app\.startup-routes$)'
```

Each executable takes `--filter <text>` to run only the cases whose name
contains the text.

The fixtures cover merge discovery and both rebase backends, including linked
worktrees, launches from subdirectories, and a rebase stopped after an earlier
commit has replayed. Assertions compare commit identities, labels and exact
stage blob IDs. Real cherry-pick, revert and mailbox conflicts stay outside
supported merge/rebase work, alongside corrupt operation metadata.

Classification covers missing stages, add/add, regular and executable modes,
unsupported source types including stage 1, result topology, text conversion
and unverifiable provenance. Literal path cases include whitespace, newlines,
leading dashes, pathspec-like names and non-UTF-8 bytes. Each selected conflict
is staged while every other index entry is compared byte for byte, including
an unrelated executable entry whose working content differs from its index.

Sentinel files detect custom filter, hook, monitor or lazy-fetch execution.
Guards are checked before materialization, clean conversion, staging and the
parent staging handoff. Inspection disables external monitors, while staging
rejects them. Partial/promisor cases include missing objects, promisor packs
and alternate stores. The tests do not fetch, change system packages or modify
a user's repository.

Fixture Git reads only a private global configuration (`GitHome` in
`tests/fixtures/GitRepository.h`), with the system configuration off, and the
suites run under `tests/with-private-xdg.sh`. The Git that `GitAdapter` starts
does not: the adapter builds its environment from the account's passwd home and
leaves the system configuration on, so it still reads `~/.gitconfig` and
`/etc/gitconfig`. `git.user-config-isolation` shows this and is registered with
`WILL_FAIL` until the adapter takes that environment from its caller.

The adapter suite includes a real 30-second operation deadline. CTest reports
case and assertion totals separately for the adapter and classification suites.

## Real mergetool parent

`MergetoolParentTests.cpp` (CTest `git.mergetool-parent`) runs
the fixture Git's `git mergetool` against the built `branch-truce` launcher and an
in-process session. The shared fixture is
`tests/fixtures/mergetool/RealParent.h`. Each parent runs in its own process
group and is killed with it when a case ends. The parent fixtures exercised
Git 2.43.0 only. `docs/mergetool-launcher.md` lists the tested configuration, the
behaviour declared compatible and the trust boundaries.

```sh
ctest --test-dir build/desktop --output-on-failure -V -R '^git\.mergetool-parent$'
```
