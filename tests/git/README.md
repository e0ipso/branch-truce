# Real Git fixtures

`GitAdapterTests.cpp` uses disposable repositories and the absolute executable
`/usr/bin/git`. It prints the selected executable and version and checks the
Git 2.43 compatibility floor. TRUCE-0031 was exercised with Git 2.43.0;
no additional Git version was exercised for that ticket.

Run from the checkout root:

```sh
/usr/bin/git --version
./tools/prepare-worktree.sh desktop
cmake --build build/desktop --parallel 2
ctest --test-dir build/desktop --output-on-failure -V \
  -R '^(git.adapter|git.conflict-classification|app.startup-routes)$'
```

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
and alternate stores. Fixture setup uses an isolated Git configuration; the
tests do not fetch, change system packages or modify a user's repository.

The adapter suite includes a real 30-second operation deadline. CTest reports
case and assertion totals separately for the adapter and classification suites.
