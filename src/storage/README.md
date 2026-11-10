# File bytes and durable storage

TextFormat decodes and serializes text with the engine's SnapshotFormat and reports typed failures. RecoveryStore reads journals and content-addressed payloads; RecoveryLease holds a store for one process and is the only way to append. DestinationWriter captures destination identity and publishes bytes through guarded filesystem operations. Every system call whose failure callers must survive goes through FileSystem, which tests replace to inject faults; FileDescriptors builds no-follow opens, whole writes and atomic replacement on it. Failures come back as a Problem from [problems/](../problems/README.md), never as a sentence.

Add low-level file-format, journal-storage and destination-write mechanisms here. Recovery replay policy belongs in recovery/; deciding when a reviewed result may publish belongs in publication/.
