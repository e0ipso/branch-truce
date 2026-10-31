# File bytes and durable storage

TextFormat preserves encoding and line endings. RecoveryStore persists journals and content-addressed payloads under a lease. DestinationWriter captures destination identity and publishes bytes through guarded filesystem operations.

Add low-level file-format, journal-storage and destination-write mechanisms here. Recovery replay policy belongs in recovery/; deciding when a reviewed result may publish belongs in publication/.
