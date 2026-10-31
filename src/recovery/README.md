# Recovery acknowledgment and replay

RecoveryJournal retains failed input and commits acknowledgment boundaries. RecoveryRecord serializes and reads complete session state. RecoveryRestore replays records and reconciles interrupted mutations without silently losing user work.

Add recovery transaction policy, replay and reconciliation here. Keep journal byte storage and leases in storage/ and ordinary session opening in app/SessionLoader.
