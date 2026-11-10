# Recovery acknowledgment and replay

RecoveryJournal commits acknowledgment boundaries under the store's lease, writes records behind the caller on a worker in order, and keeps failed input in one pending queue per file for retry. RecoveryRecord encodes a file's state as a typed record and reads each field back; RecoveryNames holds the actions, mutation facts and their names on disk. RecoveryReplay rebuilds file sessions by repeating each recorded transition, without I/O. RecoveryReconciler checks recovered files against disk and index through a RepositoryProbe, and RecoveryInventory chooses, sets aside and deletes stores. SessionIdentity holds the same-operation and captured-conflict checks that restore and publication share.

Add recovery transaction policy, replay and reconciliation here. Journal bytes, payloads and the lease are in storage/; ordinary session opening is in app/SessionLoader.
