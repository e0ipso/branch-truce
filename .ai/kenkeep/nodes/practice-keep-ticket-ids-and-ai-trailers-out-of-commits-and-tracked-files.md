---
type: practice
title: Keep ticket IDs and AI trailers out of commits and tracked files
description: >-
  Conventional Commits headers carry no TRUCE IDs or co-author/AI trailers;
  check:tracker-refs bans ticket IDs outside agent paths.
tags:
  - git
  - commits
  - ci
  - conventions
  - release
kk_schema_version: 3
kk_id: practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files
kk_derived_from: []
kk_relates_to: []
kk_depends_on: []
kk_confidence: high
---
[commitlint.config.js](../../../commitlint.config.js) extends `@commitlint/config-conventional`. The header type is one of `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`, `chore` or `revert`, in lower case. The subject has at most 72 characters and no closing period. Body and footer lines have at most 100.

A header must not contain a `TRUCE-<number>` ticket ID. semantic-release ([.releaserc.json](../../../.releaserc.json)) builds release notes and `CHANGELOG.md` from the headers, and the tracker is not published. Put the ID in a `Refs: TRUCE-0000` trailer in the body instead. Any `Co-authored-by:` trailer fails, human or not. So do `Assisted-by:`, `Generated-by:`, `Made-with:` or `Signed-off-by:` lines that name an AI tool, tool trailers such as `Copilot-Session:`, and "Generated with ..." lines that name a tool. The config turns commitlint's default ignores off. A merge, revert or `fixup!` message skips the rules only when its first line matches a default ignore and the whole message passes both custom rules.

`npm run check:tracker-refs` runs [scripts/check-tracker-refs.ts](../../../scripts/check-tracker-refs.ts). It fails when a tracked file cites a ticket ID or a path under `.ai/strikethroo/plans`. Its `EXCLUDED` list skips `.ai/`, `.agents/`, `.claude/`, `.cursor/`, `.codex/`, `skills-lock.json`, `.kkignore` and `tests/`. Knowledge nodes and test names may cite tickets. Source comments, `docs/`, `packaging/`, `tools/` and the root documents may not. CI ([.github/workflows/ci.yml](../../../.github/workflows/ci.yml)) runs commitlint over the commits of each PR or push, or only the last commit after a force push, then `npm run typecheck`, `npm run check:tracker-refs` and `npm test`. `npm test` runs `scripts/*.test.ts`, which cover the commitlint config and the checker.

Locally, [.husky/commit-msg](../../../.husky/commit-msg) runs commitlint. The shared `.git/config` sets `core.hooksPath` to the relative path `.husky/_`, and `husky` creates that directory from the `prepare` script during `npm install`. A linked worktree that never ran `npm install`, such as a sift worktree, has no `.husky/_` and no `node_modules`. Git finds no `commit-msg` hook there and accepts any message, so CI is the first check. Apply the rules by hand in those worktrees.
