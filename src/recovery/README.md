# Recovery acknowledgment and replay

RecoveryJournal commits acknowledgment boundaries under the store's lease, writes all commands and typing on one worker in request order, and holds dependent records after a failure for retry under their original transaction IDs. Command futures acknowledge only durable records; shutdown drains admitted records without a deadline. RecoveryRecord encodes a file's state as a typed record and reads each field back; RecoveryNames holds the actions, mutation facts and their names on disk. RecoveryReplay rebuilds file sessions by repeating each recorded transition, without I/O. RecoveryReconciler checks recovered files against disk and index through a RepositoryProbe, and RecoveryInventory chooses, sets aside and deletes stores. SessionIdentity holds the same-operation and captured-conflict checks that restore and publication share.

Add recovery transaction policy, replay and reconciliation here. Journal bytes, payloads and the lease are in storage/; ordinary session opening is in app/SessionLoader.

Recovery admission supports a 128-byte header, 4 MiB frames, a 32 MiB journal,
8192 transactions with IDs of at most 256 UTF-16 code units, 16 MiB of encoded
record bodies plus ID/entry accounting, 32768 payload files, and 256 MiB of
journal and payload bytes per store. A separate 256 MiB cumulative expanded-byte
budget counts every encoded payload reference before deduplication and every
full current draft, including drafts represented by short deltas. Readers
enforce each record's claimed budget while resolving payloads and reconstructing
text, and replay checks it against canonical re-encoding. This bounds repeated
references as well as delta expansion; it is logical byte accounting rather
than a process heap limit. Individual payloads remain limited to
8 MiB. Smaller injected limits exercise boundaries. Readers bound bytes before
decoding and reject stores outside this envelope. Payloads left by interrupted
writes count toward retention too. Capacity failures retain pending input and
all existing records; exact retries of complete records still flush at capacity.

Checkpoint compaction and orphan cleanup are deferred. Records depend on earlier
revisions and mutation targets, so deleting history or payloads without a complete
checkpoint would lose captures, decisions, checkpoints or uncertain Git evidence.
Long sessions therefore stop admitting new records at the first limit. There is
no automatic capacity reclamation, including after a failed payload write.
Journal format 3 records this expanded budget; earlier journals are rejected
without modification.

Inventory checks only the first integrity-checked frame within 64 KiB and its
root payload within 4096 bytes before skipping an unrelated worktree. It creates
no separate identity file. Matching or unknown identities still require full
replay, consistent capture identities and repository validation before adoption.
