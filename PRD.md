# Branch Truce product requirements

## Problem, user, and confirmed direction

The user keeps an IDE for its conflict resolver and wants to stop paying for an IDE they otherwise do not need. They need a standalone Linux app that lets them understand competing contributions, compose the intended result, and return to their ordinary work. The known audience is this user; broader audience needs remain unconfirmed.

Confirmed direction:

- Run as a standalone Linux app launched from the user's existing Git workflow.
- Work entirely offline, without an account, sign-in, subscription, or payment flow. The user opens directly into their conflict-resolution work.
- Resolve text conflicts encountered while merging and rebasing in the first release. Other operations, including cherry-picking and applying stashed changes, remain unconfirmed scope.
- Present all currently conflicted files together in one session, with a useful file list and visible progress.
- Show two read-only source versions around an editable result in the center. Clearly identify each source for the current merge or rebase so the user can tell whose contribution they are choosing.
- Let the user choose individual lines or whole sections from either source and edit the result directly. All three are required for the first release. Code checks remain in their usual editor.
- Use mouse controls with basic editing shortcuts in the first release. Full keyboard operation belongs to the second iteration.
- Search within files, wrap long lines for reading, and reveal spaces or tabs in the first release.
- Let the user inspect the common earlier version on demand, when available, without keeping an additional pane permanently visible.
- Let the user save unfinished work without declaring the file resolved. A separate "Mark resolved" action saves the current reviewed result and marks the file resolved in the user's existing Git workflow, without requiring a separate Save first.
- Resume unfinished work after closing the app or a crash, restoring edits that were not manually saved and the decisions still outstanding. Recovered unsaved edits remain visibly unsaved.
- Let the user save reviewed results and return to their usual workflow to continue the operation and commit.
- Handle text conflicts first. Unsupported renamed, deleted, and image cases must prominently say they require handling outside the app, both beside the file and in the session summary. They never count as resolved merely because the app cannot handle them.

Success means the user can replace the resolver they retain today and understand the result they take back to their usual workflow. They can complete supported work, account for unfinished or unsupported files, and resume an interrupted session. Completing every visible decision does not establish that the content is correct.

Detecting changes made in other apps and offering a comparison to reconcile them are outside the first release.

## Conflict-resolution experience

The confirmed first-release capabilities, including the source arrangement and understandable source identities, are required. Second-iteration capabilities and candidate conveniences remain separate. Additional recommendations are labeled explicitly.

### Understand the versions before choosing

The first release shows two read-only source versions around an editable result in the center. Label all three clearly. Identify each source truthfully for the user's task, using meaningful names and version descriptions when branch names alone are insufficient. Left/right position and labels such as "mine" cannot carry the whole explanation. If identity is uncertain, make that uncertainty visible.

Show corresponding passages together with enough surrounding text to judge them. Distinguish additions, removals, modifications, and competing changes using more than color. Word-level highlighting should reveal differences within a line without suggesting that every highlighted word has a separate acceptance control.

Let the user compare the result with either original source throughout review. Access to the common earlier version on demand is required for the first release when that version is available. The user can show it for context and hide it to return to the main comparison. Explain when that context is unavailable. Do not consume permanent reading space with it unless the user chooses to view it.

### Make choices without losing intent

Provide an accept arrow and an ignore X for each side's change, with understandable labels and mouse controls. Accepting the left contribution leaves the right contribution undecided; accepting right behaves symmetrically. A distinct "resolve using this side" action accepts that contribution and rejects the competing contribution for the selected region. Its effect must be clear before use.

Ignore means deciding not to apply the selected contribution. It leaves the current result text unchanged, including manual edits, and removes only that contribution from the undecided choices. The competing contribution, if any, stays undecided. Ignore is available for an undecided contribution; it does not remove text already accepted. To change an accepted choice to Ignore, the user first undoes the acceptance. Undoing Ignore makes that contribution undecided again.

Allow both contributions and subsequent result editing. Show which contribution comes first and let the user choose the other order when order affects the combined text. Do not disguise omitted overlapping edits or duplicated text as a clean combined decision. The user can inspect and revise the outcome before saving.

A region can contain several judgments. The user can choose individual lines from either source, choose a whole section, and edit the result directly. Line choices make it possible to retain selected lines from both contributions without first accepting the whole section. Show which lines or section a choice affects and leave other choices available. Additional word-level selection controls remain a candidate convenience. Choosing a source after manual editing must make any replacement of those edits understandable and reversible.

Undo and redo should restore both the visible text and whether the affected choices remain undecided. Include manual edits, individual-line choices, single-region choices, bulk choices, and whole-file choices. Recommend retaining this ability through saving while the current session stays open. Restoring unfinished edits and decisions after leaving is required; retaining the complete undo history after reopening remains a candidate convenience.

### Preserve the demonstrated broader choices

Offer bulk application of non-conflicting changes from the left, right, or both. State the affected file and changes before applying the choice. A bulk non-conflict action must leave competing choices and unrelated manual edits alone.

Offer a whole-file choice using either source. Explain that it replaces the result for that file and gives up the other source's contributions, including any manual changes it would replace. Let the user inspect the proposed replacement, undo it, and review before saving.

Keep next/previous undecided-change navigation distinct from navigation through all differences. Let the user reach the next unfinished file and return to saved or reviewed files. Closing and reopening the file list does not finish the session or lose the user's place.

## Reading and supported text challenges

Search within files, line wrapping, and controls to reveal spaces or tabs are required for the first release. Let the user search the sources and result with a clear search scope and wrap long lines to read them without changing the text. Keep sufficient context around distant changes. Repeated passages must not make a choice appear attached to the wrong passage. When correspondence is uncertain, keep the original context accessible and avoid implying certainty.

Let the user show or hide visible indications of spaces and tabs while reviewing a difference. Changes involving only whitespace must remain discoverable, and hiding the indications must not mark those changes resolved. These are reading capabilities, not code checks.

Recommended supported examples include competing edits within one line, additions beside other additions, removals within otherwise supported text, mixed contributions within a region, empty text, repeated passages, and changes far apart in a file. Removing text within a supported file does not mean handling a deleted-file conflict.

Empty content, unavailable earlier context, and content that cannot be opened are different experiences. Explain which applies. Difficult cases and usable reading limits still need validation against the user's files; this draft makes no size or speed promise.

The first release supports mouse controls and basic editing shortcuts for working in the result, including copying, pasting, undo, and redo. Full keyboard operation across files, comparisons, conflict choices, review, and saving belongs to the second iteration. Recommend visible focus, text labels, and status indicators whose meaning does not depend on color. Further assistive reading needs remain unconfirmed.

## User journeys

These journeys show how the confirmed needs and recommended interactions fit together.

### Resolve mixed work and return to the usual workflow

The user launches Branch Truce from their existing Git workflow while their work has several conflicted files. They enter the session directly, without sign-in or payment, and can work without an internet connection. The session shows every current file, identifies supported text work, and prominently calls out outside-app work. The user opens a supported file, checks the source identities, and inspects the first undecided region.

They accept one contribution, leave the other undecided, then choose both in a different region. They inspect the order and edit the result to retain only the intended text. They can save their progress while work remains. Once the result is ready, they compare it with its sources and review the complete file. They choose "Mark resolved" when satisfied. The app saves the current result and marks the file resolved in their existing Git workflow. If source choices remain undecided after manual editing, the user first confirms that the reviewed result is intentional.

They repeat this across the current session. Before leaving, they see what is saved and reviewed and what remains outstanding. They run code checks in their regular editor and return saved reviewed results to their usual workflow for continuation and commit. Branch Truce does not claim to have checked the code or finished that wider work.

### Recover from an overbroad choice

The user manually edits a region and later considers replacing the whole file with one source. The app explains the replacement and lets them inspect it. After trying it, they undo and recover the edited result and the previous undecided choices. They can continue reviewing without reconstructing their work from memory.

### Finish supported work while an unsupported file remains

The user saves and reviews two text files. A renamed file and an image conflict still require outside handling. Their file notices remain prominent, and the session says that supported text work is complete while two files remain outstanding. Leaving the app does not relabel those files or imply that the wider operation is ready to continue.

## Progress, saving, and handoff

Recommend plain, separate answers to the user's questions: What is still undecided? What has changed without being saved? What content is saved? Which latest results have I reviewed? What needs handling outside the app? These meanings can coexist for one file. A saved draft can still contain undecided work, and completed decisions can still need review.

Save preserves progress without declaring the file resolved or reviewed. The user can save incomplete work, which remains visibly unfinished.

After reviewing the result, the user chooses "Mark resolved." This action saves the latest result and marks the file resolved in the existing Git workflow. A separate Save is not required, and merely changing the app's progress indicator is not enough. The action does not continue the wider operation or create a commit.

If source choices remain undecided, ask the user to confirm that the current result is their intentional, reviewed resolution before saving and marking the file resolved. Cancelling leaves the current text and choices unchanged. Confirming accepts the reviewed result as the resolution for the file; the user does not have to make additional source selections after resolving the text by hand.

A later edit means the changed content needs review again and must not retain a misleading resolved status.

Before handoff, show the current session's saved results, outstanding decisions, unsaved changes, review still needed, failures, and outside-app work. Offer a route to each outstanding file. Any ordinary finish action must warn about unfinished work. The user can deliberately leave to handle it elsewhere without a success claim.

The session covers the work launched into it. It makes no promise about conflicts that might appear after the user continues elsewhere. Continuing and committing remain in the user's usual workflow.

## Failure and recovery expectations

Restoring unfinished work after closing the app or a crash is required. On return, the user can continue with their edits, including those not manually saved, and the decisions still outstanding. Recovered edits that were not saved remain visibly unsaved; recovery does not claim that they were reviewed or resolved. Deliberately discarded work must not be presented as the current result.

The user must be able to distinguish unsaved, saved-but-unresolved, and resolved work. Merely opening or inspecting work must not change the user's content or make choices for them. Switching files or closing the app must not silently lose unsaved edits. Explain save, leave, and discard consequences, including which earlier saves will remain.

If saving fails, say which file failed, keep its edited result available, leave its unsaved warning visible, and offer retry or a safe way to remain in the session. Saving one file successfully must not conceal another failure.

If saving during "Mark resolved" fails, leave the file unresolved in the app and the existing Git workflow. If the result saves but marking the file resolved cannot finish, show that it is saved but still unresolved and offer retry. Show the file as resolved only after both saving and marking it resolved succeed.

If a file cannot be read or the session cannot show expected work, explain the problem and keep it outstanding. A blank view is not evidence of an empty file, no conflicts, or completion. If recovery cannot restore expected work, explain what is missing and keep the affected file outstanding. Never produce a false claim of recovered, saved, reviewed, or resolved work.

## Observable acceptance examples

Checks of confirmed requirements apply unconditionally, including the source arrangement and source identities. Any explicitly labeled recommendations remain proposals.

1. **All current work stays visible.** With three supported files and one unsupported deleted-file case, the file list includes all four. Saving the three supported results leaves a prominent session warning and a file-level notice requiring outside handling.

2. **Choices work at both sizes.** Within one conflict, the user can take selected lines from left and right, choose a whole section from either side, and edit the result directly. A line choice leaves other lines available for a separate decision. Accepting left leaves right undecided; resolving the region using left alone explicitly rejects right. Undo restores the previous result and choice. These actions leave other regions alone.

3. **Both has an inspectable order.** With two competing additions, the user can see and change their order, remove duplication, and edit within the combined region before saving. The app does not claim that taking both proves correctness.

4. **Broad choices are recoverable.** After a manual edit, the user previews a whole-file choice and sees that the edit would be replaced. Undo restores it. A separate bulk non-conflict choice affects only its stated scope and can also be undone.

5. **Saving and resolving are separate choices.** The user can save a file with undecided changes and continue working; it remains visibly unresolved. With a reviewed result that still has unsaved edits, the user chooses "Mark resolved." The latest result is saved and the file is marked resolved in their existing Git workflow without a prior Save. Save alone does not mark it reviewed or resolved. Editing the result again makes the changed content need review again.

6. **Reading supports difficult text.** With repeated passages, a long line, and a whitespace-only difference, the user can identify the intended region, search within the sources and result with a known scope, wrap the long line, and reveal spaces or tabs. Changing these reading aids leaves the text and resolution decisions unchanged. Sources remain read-only.

7. **Source views, identity, and earlier context are required.** During both merging and rebasing, two read-only source views surround the editable result in the center. Source descriptions let the user identify each contribution even without useful branch names. The user can show the common earlier version on demand and hide it without losing their current result or choices. Unavailable earlier context is explained rather than shown as an invented empty version.

8. **Mouse controls and editing shortcuts are usable.** The user can select a file, inspect sources, accept or ignore, review, and save using mouse controls. While editing the result, familiar shortcuts support copying, pasting, undo, and redo. Full keyboard operation is not a first-release acceptance requirement. Recommended focus and outstanding-work indicators remain understandable without their colors.

9. **Failures stay outstanding.** A failed save leaves the edited result available and unsaved. If it fails during "Mark resolved," the file remains unresolved in both the app and the existing Git workflow. If saving succeeds but marking resolved fails, the file is shown as saved and unresolved. A file that cannot open keeps an explanation and outstanding warning rather than appearing complete.

10. **Handoff describes only this session.** With one saved reviewed file, one saved unfinished draft, and one unsaved result, the summary distinguishes all three and warns before finishing. It directs the user to outstanding work and makes no claim about later conflicts or checks in their usual editor.

11. **Unfinished work resumes.** With unsaved edits and undecided changes in several files, closing and reopening the app restores the edits and outstanding decisions. The same holds after a crash. Recovered unsaved edits still need saving, and unfinished files remain unresolved.

12. **Access requires no account or connection.** Without an internet connection or an account, the user can launch a session, compare sources, choose changes, edit, save, mark files resolved, and resume unfinished work. No sign-in, subscription, or payment flow interrupts these actions.

13. **Ignore decides against a contribution without changing the result.** With a proposed line removal still undecided, choosing Ignore keeps the line in the result and marks that contribution decided. Other undecided choices remain. Existing manual edits stay unchanged. Undo restores the ignored contribution to undecided. If the removal was already accepted, the user first undoes that acceptance before choosing Ignore.

14. **Manual resolution can finish with source choices still undecided.** The user edits the intended result directly and chooses "Mark resolved" while some source choices remain undecided. The app asks them to confirm the current result is intentional. Cancelling keeps the text and choices unchanged. Confirming saves that result and marks the file resolved in their Git workflow without requiring further source selections.

## Second iteration

Full keyboard operation across file navigation, source comparison, conflict choices, review, and saving belongs to the second iteration. The first release retains mouse controls and basic editing shortcuts.

## Candidate conveniences

Additional word-level picks, remembered pane arrangements, an alternate result position, complete undo history after reopening, and automatic movement after a choice may reduce effort. Validate them against real conflicts after the reference interaction and safety requirements are usable. Automatic movement must not interrupt combining or editing a region. None is agreed scope; individual-line choices, whole-section choices, direct editing, and recovery of unfinished edits and decisions are already required.
