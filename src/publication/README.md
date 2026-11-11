# Reviewed result publication

ResolutionPublisher coordinates review, text conversion, guarded destination writing and verified Git staging or mergetool handoff for one exact result revision; one table names what finishes each of its steps. ParentStaging holds the path a waiting git mergetool parent stages and the written revision offered to it. A failed or uncertain step remains outstanding.

Add result-publication sequencing and verification here. Implement filesystem writes in storage/ and Git operations in git/; Save checkpoints and recovery acknowledgment are separate responsibilities.
