# Git mergetool launch and handoff

MergetoolLauncher is the blocking child invoked by Git. MergetoolProtocol handles session identity and local messages. MergetoolSession connects launchers to the app and tracks published revisions until the parent can stage them.

Add launcher transport, peer validation and Git-parent handoff logic here. Use publication/ for the reviewed result sequence and git/ for repository inspection.
