# Proposal: keep rebalance out of wave capture

## Target

Add the following rule to the `TASK` instructions in section 4, "Wave knowledge
capture", of the installed sift-drain prompt at
`.agents/skills/sift-drain/references/wave-gate.md`, after the paragraph about
curation conflicts. Carry the same boundary into the kenkeep capture skills so
a capture invoked by a wave gate cannot reorganize the knowledge base implicitly.

## Proposed rule

> Do not apply kenkeep rebalance actions during a wave-gate capture. If a
> rebalance trigger reports structural actions, report the actions and their
> evidence as a dx candidate for the orchestrator. Apply any structural
> rebalance only in a dedicated, supervised maintenance pass outside wave
> gates.

Wave capture already handles durable facts and conservative conflict resolution,
but it does not say what to do with structural triggers. An explicit boundary
keeps a routine capture from moving unrelated knowledge while still preserving
the trigger's findings for review.

## Local decision

The repository's knowledge index currently lists 31 root nodes and no branches.
Defer a supervised structural rebalance to a dedicated maintenance pass outside
this drain's wave gates. This proposal does not authorize a rebalance or change
installed skills or knowledge entries.
