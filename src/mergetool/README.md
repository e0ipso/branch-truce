# Git mergetool launch and handoff

The protocol library (`branch_truce_mergetool_protocol`) holds what the launcher and the window share: MergetoolMessages, the typed and versioned frames; MergetoolProtocol, the session identity, socket and intake paths and the launcher's file access; and MergetoolServer, the window's guarded listener. MergetoolLauncher is the blocking child invoked by Git. MergetoolSession queues attached launchers and tracks published revisions until the parent can stage them.

Add message kinds and their validation in MergetoolMessages, transport rules in MergetoolServer and Git-parent handoff logic in MergetoolSession. Use publication/ for the reviewed result sequence and git/ for repository inspection.
