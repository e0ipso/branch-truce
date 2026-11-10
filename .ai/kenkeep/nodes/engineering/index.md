# kenkeep Index: engineering

↑ Parent: [kenkeep](../index.md)

> kenkeep navigation: the injected body above is the root index node, the top-level catalog of branches and root-level leaves. Do not expect the whole knowledge base here; descend on demand. Read the root index node, pick one or more branches whose intent and tags match your task (several branches can be relevant), and read those branch `index.md` nodes. Descend further only where the task needs it, opening only the leaves you have confirmed are relevant. Follow each leaf's `relates_to` and `depends_on` cross edges to reach related leaves in other branches. You decide how deep to go per branch.

> This index only orients you; leaves hold the durable guidance. Open at least one relevant leaf before acting.

## Subfolders
_None._

## Conventions (how we build)
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) to learn about: TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable. #documentation #techspec #prd
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) to learn about: Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files. #git #commits #ci #conventions #release

## Components (what exists)
_None yet._

## By topic

### #ci
- Open [**Keep the qmllint target at zero warnings**](../ui/practice-keep-the-qmllint-target-at-zero-warnings.md) — src/ui/.qmllint.ini makes branch_truce_ui_qmllint fail on any warning; type QML properties concretely, anchor at use sites.
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #commits
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #conventions
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #documentation
- Open [**Keep PRD.md focused on user needs**](../product/practice-keep-prd-focused-on-user-needs.md) — PRD.md describes user needs, interactions, and capabilities without technical or implementation details.
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.
### #git
- Open [**Mergetool launcher decisions and reloads**](../git/mergetool/map-mergetool-launcher-decisions-and-reloads.md) — Launcher replies control parent staging; reloads reassign waiting paths and revalidate copied launch results.
- Open [**Mergetool socket lookup and access**](../git/mergetool/map-mergetool-socket-lookup-and-access.md) — GUI and launcher share a short owner-only socket directory with a locked random fallback.
- Open [**Real Git parent compatibility and fixture isolation**](../git/mergetool/map-real-git-parent-compatibility-and-fixture-isolation.md) — Real Git parent tests bound the compatibility claim and require short private runtime directories.
### #prd
- Open [**Keep PRD.md focused on user needs**](../product/practice-keep-prd-focused-on-user-needs.md) — PRD.md describes user needs, interactions, and capabilities without technical or implementation details.
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.
### #release
- Open [**Keep ticket IDs and AI trailers out of commits and tracked files**](practice-keep-ticket-ids-and-ai-trailers-out-of-commits-and-tracked-files.md) — Sift ticket references stay inside Sift; commitlint and check:tracker-refs enforce this for commit messages and tracked files.
### #techspec
- Open [**Design desktop apps around screens and components**](../architecture/practice-design-desktop-apps-around-screens-and-components.md) — Define screens and components, then build loosely coupled implementations using each desktop environment's idiomatic UI kit.
- Open [**Keep TechSpecs and PRDs independent**](practice-keep-techspecs-and-prds-independent.md) — TechSpecs explain how to build; PRDs explain what to build and why. Keep both separate and independently understandable.