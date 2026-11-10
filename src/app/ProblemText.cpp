// Problem text turns the typed failures of Git, storage, recovery and the mergetool into translated sentences.
#include "ProblemText.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>

namespace branch_truce::problem_text {
namespace {
// The translation context of every problem sentence.
struct Text {
    Q_DECLARE_TR_FUNCTIONS(Problem)
};

// How a problem's sentence takes the problem it wraps.
enum class Wrap {
    None, // It wraps nothing.
    Argument, // The wrapped sentence is its last argument.
    After, // It follows the wrapped sentence after a semicolon.
    Output // The wrapped sentence comes first, then its one argument after a colon.
};

Wrap wrap(ProblemCode code)
{
    switch (code) {
    case ProblemCode::PublicationCleanupFailed:
    case ProblemCode::ClassificationInspectionFailed:
    case ProblemCode::ClassificationUnsupportedOperation:
    case ProblemCode::ClassificationStageNotEditable:
    case ProblemCode::RecoveryStagingNotRetained:
    case ProblemCode::RecoveryFileUnvalidated: return Wrap::Argument;
    case ProblemCode::GitCleanupFailed:
    case ProblemCode::GitReinspectionTimedOut:
    case ProblemCode::GitExecutableChangedBeforeReinspection:
    case ProblemCode::GitReinspectionTooLarge:
    case ProblemCode::GitReinspectionFailed: return Wrap::After;
    case ProblemCode::GitFailedWithOutput: return Wrap::Output;
    default: return Wrap::None;
    }
}

// Whether the sentence asks for work outside Branch Truce; it then starts with "Outside app: ".
bool outsideApp(ProblemCode code)
{
    switch (code) {
    case ProblemCode::TextUnsupportedEncoding:
    case ProblemCode::TextUnsupportedEol:
    case ProblemCode::TextInvalidUtf8:
    case ProblemCode::TextMissingByteOrderMark:
    case ProblemCode::TextContradictoryByteOrderMark:
    case ProblemCode::TextTruncatedUtf16:
    case ProblemCode::TextInvalidSurrogatePair:
    case ProblemCode::TextUnpairedSurrogate:
    case ProblemCode::TextControlCharacters:
    case ProblemCode::TextBareCarriageReturn:
    case ProblemCode::TextMixedNewlines:
    case ProblemCode::TextNoRoundTrip:
    case ProblemCode::TextInvalidEditable:
    case ProblemCode::TextSerializedNoRoundTrip:
    case ProblemCode::GitWorktreeRootResponse:
    case ProblemCode::GitObjectDirectoryResponse:
    case ProblemCode::GitObjectStoreUninspectable:
    case ProblemCode::GitTooManyAlternates:
    case ProblemCode::GitPacksUninspectable:
    case ProblemCode::GitPromisorPacks:
    case ProblemCode::GitAlternatesUninspectable:
    case ProblemCode::GitAlternatesUnreadable:
    case ProblemCode::GitAlternatePathLossy:
    case ProblemCode::GitAttributeInspection:
    case ProblemCode::GitHooksResponse:
    case ProblemCode::GitHooksPathLossy:
    case ProblemCode::GitHooksUninspectable:
    case ProblemCode::GitPostIndexChangeHook:
    case ProblemCode::GitHooksPathNotDirectory:
    case ProblemCode::GitPartialOrMonitor:
    case ProblemCode::GitCustomFilter:
    case ProblemCode::GitFilterAttributes:
    case ProblemCode::GitFilterPath:
    case ProblemCode::DiscoverySequencerUnsupported:
    case ProblemCode::DiscoveryMultipleMergeHeads:
    case ProblemCode::DiscoveryApplyMailbox:
    case ProblemCode::DiscoveryUnverifiedIndex:
    case ProblemCode::DiscoveryStageProvenance:
    case ProblemCode::DiscoveryMissingStage:
    case ProblemCode::DiscoveryMergeCommitReplay:
    case ProblemCode::DiscoveryMultipleBases:
    case ProblemCode::ConversionUnsupportedAttributes:
    case ProblemCode::ConversionStageEntries:
    case ProblemCode::ConversionModeDisagrees:
    case ProblemCode::ConversionSourceIdentity:
    case ProblemCode::ConversionInvalidPath:
    case ProblemCode::ConversionMissingSnapshot:
    case ProblemCode::ConversionSnapshotBytes:
    case ProblemCode::ConversionWorktreeChanged:
    case ProblemCode::ConversionConfigurationChanged:
    case ProblemCode::ConversionPolicyChangedReload:
    case ProblemCode::ConversionChangesBytes:
    case ProblemCode::ConversionNoRoundTrip:
    case ProblemCode::ConversionStagedIdentity:
    case ProblemCode::ConversionStagedBytes:
    case ProblemCode::ConversionStagedCheckout:
    case ProblemCode::ConversionPolicyChanged:
    case ProblemCode::ClassificationCancelled:
    case ProblemCode::ClassificationBusy:
    case ProblemCode::ClassificationChanged:
    case ProblemCode::ClassificationResultChanged:
    case ProblemCode::ClassificationConfigurationChanged:
    case ProblemCode::ClassificationInspectionFailed:
    case ProblemCode::ClassificationInvalidStages:
    case ProblemCode::ClassificationNonRegular:
    case ProblemCode::ClassificationMissingSide:
    case ProblemCode::ClassificationModeConflict:
    case ProblemCode::ClassificationTopology:
    case ProblemCode::ClassificationUnsupportedOperation:
    case ProblemCode::ClassificationConversionBusy:
    case ProblemCode::ClassificationStageNotEditable:
    case ProblemCode::ClassificationPoliciesChanged:
    case ProblemCode::ClassificationResultTopology: return true;
    default: return false;
    }
}

QString format(ProblemCode code)
{
    switch (code) {
    case ProblemCode::None: return {};
    case ProblemCode::TextUnsupportedEncoding: return Text::tr("unsupported working-tree-encoding: %1");
    case ProblemCode::TextUnsupportedEol: return Text::tr("unsupported eol attribute");
    case ProblemCode::TextInvalidUtf8: return Text::tr("invalid UTF-8 or an unattributed unsupported encoding");
    case ProblemCode::TextMissingByteOrderMark: return Text::tr("UTF-16 requires a BOM");
    case ProblemCode::TextContradictoryByteOrderMark: return Text::tr("UTF-16 BOM contradicts its encoding");
    case ProblemCode::TextTruncatedUtf16: return Text::tr("truncated UTF-16 code unit");
    case ProblemCode::TextInvalidSurrogatePair: return Text::tr("invalid UTF-16 surrogate pair");
    case ProblemCode::TextUnpairedSurrogate: return Text::tr("unpaired UTF-16 surrogate");
    case ProblemCode::TextControlCharacters: return Text::tr("binary/control characters are unsupported");
    case ProblemCode::TextBareCarriageReturn: return Text::tr("bare CR newlines are unsupported");
    case ProblemCode::TextMixedNewlines: return Text::tr("mixed LF and CRLF newlines are unsupported");
    case ProblemCode::TextNoRoundTrip: return Text::tr("text decoding does not round-trip");
    case ProblemCode::TextInvalidEditable:
        return Text::tr("editable text must be valid UTF-8 with LF separators and no binary/control characters");
    case ProblemCode::TextSerializedNoRoundTrip: return Text::tr("serialized text or BOM does not round-trip");
    case ProblemCode::DestinationInvalidPath:
        return Text::tr("Expected canonical worktree root and exact relative destination bytes");
    case ProblemCode::DestinationSymlink: return Text::tr("Destination is a symlink");
    case ProblemCode::DestinationDirectorySymlink: return Text::tr("Destination directory is a symlink");
    case ProblemCode::DestinationReplacedBySymlink: return Text::tr("Destination was replaced by a symlink");
    case ProblemCode::DestinationAncestorNotDirectory: return Text::tr("Destination ancestor is not a directory");
    case ProblemCode::DestinationNotRegular: return Text::tr("Destination must be a regular file");
    case ProblemCode::DestinationSizeUnrepresentable: return Text::tr("Destination size cannot be represented");
    case ProblemCode::DestinationDirectoryChangedWhileOpening:
        return Text::tr("Destination directory changed while opening");
    case ProblemCode::DestinationChangedWhileOpening: return Text::tr("Destination changed while opening");
    case ProblemCode::DestinationShrank: return Text::tr("Destination shrank while reading");
    case ProblemCode::DestinationChangedWhileReading: return Text::tr("Destination changed while reading");
    case ProblemCode::DestinationDirectoryChanged:
        return Text::tr("Destination directory identity or permissions changed");
    case ProblemCode::DestinationIdentityChanged: return Text::tr("Destination file identity changed");
    case ProblemCode::DestinationContentChanged: return Text::tr("Destination content or permissions changed");
    case ProblemCode::DestinationChangedWhileReopening: return Text::tr("Destination changed while reopening");
    case ProblemCode::DestinationChangedDuringPermissionCheck:
        return Text::tr("Destination changed during permission check");
    case ProblemCode::PublicationInvalidRequest:
        return Text::tr("Mutation admission and an agreed matching regular-file Git mode are required");
    case ProblemCode::PublicationOwnership:
        return Text::tr("Publication requires an owned file with one link and no special permission bits");
    case ProblemCode::PublicationExtendedAttributes:
        return Text::tr("Publication cannot preserve extended attributes or ACLs for this destination");
    case ProblemCode::PublicationSelinuxLabel:
        return Text::tr("Publication cannot give the replacement the destination's SELinux label");
    case ProblemCode::PublicationCancelled: return Text::tr("Publication cancelled before mutation");
    case ProblemCode::PublicationNoProgress: return Text::tr("Replacement write made no progress");
    case ProblemCode::PublicationReplacementChanged: return Text::tr("Replacement bytes changed before publication");
    case ProblemCode::PublicationUnsupportedFilesystem:
        return Text::tr("Filesystem does not support atomic destination exchange");
    case ProblemCode::PublicationUnverified:
        return Text::tr("Published destination identity or bytes could not be verified");
    case ProblemCode::PublicationCleanupFailed: return Text::tr("%1; could not remove private publication files");
    case ProblemCode::PublicationCleanupIncomplete:
        return Text::tr("Published result verified; private publication cleanup is incomplete");
    case ProblemCode::SystemInspectDirectory: return Text::tr("Cannot inspect destination directory: %1");
    case ProblemCode::SystemOpenDirectory: return Text::tr("Cannot open destination directory: %1");
    case ProblemCode::SystemInspectOpenedDirectory: return Text::tr("Cannot inspect opened destination directory: %1");
    case ProblemCode::SystemInspectDestination: return Text::tr("Cannot inspect destination: %1");
    case ProblemCode::SystemReadDestination: return Text::tr("Cannot read destination: %1");
    case ProblemCode::SystemFinishReading: return Text::tr("Cannot finish reading destination: %1");
    case ProblemCode::SystemReinspectDestination: return Text::tr("Cannot reinspect destination: %1");
    case ProblemCode::SystemReopenRoot: return Text::tr("Cannot reopen filesystem root: %1");
    case ProblemCode::SystemReinspectEntry: return Text::tr("Cannot reinspect destination entry: %1");
    case ProblemCode::SystemReopenEntry: return Text::tr("Cannot reopen destination entry: %1");
    case ProblemCode::SystemInspectAttributes: return Text::tr("Cannot inspect destination extended attributes: %1");
    case ProblemCode::SystemReadLabel: return Text::tr("Cannot read the destination SELinux label: %1");
    case ProblemCode::SystemInspectFile: return Text::tr("Cannot inspect destination file: %1");
    case ProblemCode::SystemOpenFile: return Text::tr("Cannot open destination file: %1");
    case ProblemCode::SystemReopenDestination: return Text::tr("Cannot reopen destination: %1");
    case ProblemCode::SystemNotWritable: return Text::tr("Destination is not writable: %1");
    case ProblemCode::SystemInspectWritable: return Text::tr("Cannot inspect writable destination: %1");
    case ProblemCode::SystemCreatePrivateDirectory: return Text::tr("Cannot create private publication directory: %1");
    case ProblemCode::SystemCreateReplacement: return Text::tr("Cannot create replacement file: %1");
    case ProblemCode::SystemWriteReplacement: return Text::tr("Cannot write complete replacement: %1");
    case ProblemCode::SystemPreservePermissions: return Text::tr("Cannot preserve destination permissions: %1");
    case ProblemCode::SystemSyncReplacement: return Text::tr("Cannot sync replacement: %1");
    case ProblemCode::SystemExchangeUncertain: return Text::tr("Atomic exchange outcome is uncertain: %1");
    case ProblemCode::SystemExchange: return Text::tr("Cannot exchange destination: %1");
    case ProblemCode::SystemSyncPublished: return Text::tr("Cannot sync published destination: %1");
    case ProblemCode::RecoveryPayloadTooLarge:
        return Text::tr("Recovery payload of %1 bytes exceeds the %2 byte limit; pending input is retained.");
    case ProblemCode::RecoveryPayloadUnsafe: return Text::tr("Recovery payload is missing or unsafe.");
    case ProblemCode::RecoveryPayloadIntegrity: return Text::tr("Recovery payload integrity check failed.");
    case ProblemCode::RecoveryJournalUnsafe: return Text::tr("Recovery journal is unavailable or unsafe.");
    case ProblemCode::RecoveryJournalIncomplete: return Text::tr("Could not read the complete recovery journal.");
    case ProblemCode::RecoveryJournalNewer: return Text::tr("Recovery journal was written by a newer Branch Truce.");
    case ProblemCode::RecoveryJournalVersion: return Text::tr("Unknown or corrupt recovery journal version.");
    case ProblemCode::RecoveryRecordIntegrity: return Text::tr("Recovery record integrity check failed.");
    case ProblemCode::RecoveryRecordSchema: return Text::tr("Recovery record schema or ordering is invalid.");
    case ProblemCode::RecoveryTransactionRepeated: return Text::tr("Recovery journal repeats a transaction.");
    case ProblemCode::RecoveryStorageLocation:
        return Text::tr("Recovery storage needs an absolute app-owned location.");
    case ProblemCode::RecoveryTransactionInvalid: return Text::tr("Invalid recovery transaction.");
    case ProblemCode::RecoveryStorageCreate:
        return Text::tr("Could not create private recovery storage; retry after repairing its location.");
    case ProblemCode::RecoveryJournalInitialize: return Text::tr("Could not initialize the recovery journal.");
    case ProblemCode::RecoveryCommitBoundary: return Text::tr("Could not open the recovery commit boundary.");
    case ProblemCode::RecoveryTransactionReused:
        return Text::tr("Recovery transaction identity was reused for different work.");
    case ProblemCode::RecoveryPendingFlush: return Text::tr("Could not flush the pending recovery transaction; retry.");
    case ProblemCode::RecoveryPayloadIdentity: return Text::tr("Invalid recovery payload identity.");
    case ProblemCode::RecoveryPayloadCommit:
        return Text::tr("Could not commit a recovery payload; pending input is retained.");
    case ProblemCode::RecoveryStorageSync:
        return Text::tr("Could not sync recovery storage; pending input is retained for retry.");
    case ProblemCode::RecoveryCommitFailed:
        return Text::tr("Recovery commit failed; pending input is retained for retry.");
    case ProblemCode::GitInvalidRequest:
        return Text::tr("An absolute directory and trusted executable outside it are required");
    case ProblemCode::GitUnknownOperation: return Text::tr("Unknown Git operation");
    case ProblemCode::GitInvalidOperands: return Text::tr("Invalid operands for the named Git operation");
    case ProblemCode::GitInputTooLarge:
        return Text::tr("Canonicalization input has %1 bytes, exceeding the %2 byte input limit");
    case ProblemCode::GitCleanupFailed: return Text::tr("could not remove private conversion objects: %1");
    case ProblemCode::GitCancelled: return Text::tr("Git request cancelled");
    case ProblemCode::GitStartRefused: return Text::tr("The Git request could not start");
    case ProblemCode::GitInputNotSent: return Text::tr("Could not send Git operation input");
    case ProblemCode::GitDeadline: return Text::tr("Git exceeded the %1 second deadline");
    case ProblemCode::GitReinspectionTimedOut: return Text::tr("Index reinspection timed out");
    case ProblemCode::GitLaunchFailed: return Text::tr("%1");
    case ProblemCode::GitProcessFailed: return Text::tr("%1");
    case ProblemCode::GitStartFailed: return Text::tr("Could not start Git: %1");
    case ProblemCode::GitLaunchPipe: return Text::tr("Could not create the Git launch error pipe");
    case ProblemCode::GitExecutableChangedBeforeReinspection:
        return Text::tr("Selected executable changed before reinspection");
    case ProblemCode::GitExecutableChanged: return Text::tr("Selected executable changed or is no longer trusted");
    case ProblemCode::GitReinspectionTooLarge: return Text::tr("Index reinspection exceeded %1 bytes");
    case ProblemCode::GitOutputLimit: return Text::tr("Git output exceeded %1 bytes; captured prefix is incomplete");
    case ProblemCode::GitReinspectionFailed: return Text::tr("Index reinspection failed; mutation remains uncertain");
    case ProblemCode::GitExitCode: return Text::tr("Git exited with code %1");
    case ProblemCode::GitCrashed: return Text::tr("Git process crashed");
    case ProblemCode::GitNotGit: return Text::tr("Selected executable did not identify itself as Git");
    case ProblemCode::GitWorktreeRootResponse: return Text::tr("invalid worktree root response");
    case ProblemCode::GitExecutableInRepository:
        return Text::tr("The trusted Git executable must be outside the repository");
    case ProblemCode::GitStagingNotRegular:
        return Text::tr("Staging requires one regular file without symlink traversal");
    case ProblemCode::GitObjectDirectoryResponse: return Text::tr("invalid object directory response");
    case ProblemCode::GitObjectStoreUninspectable: return Text::tr("object storage cannot be inspected offline");
    case ProblemCode::GitTooManyAlternates: return Text::tr("too many alternate object stores to inspect");
    case ProblemCode::GitPacksUninspectable: return Text::tr("object packs cannot be inspected offline");
    case ProblemCode::GitPromisorPacks: return Text::tr("promisor object packs require unsupported lazy fetching");
    case ProblemCode::GitAlternatesUninspectable: return Text::tr("alternate object stores cannot be inspected");
    case ProblemCode::GitAlternatesUnreadable:
        return Text::tr("alternate object store list is unreadable or oversized");
    case ProblemCode::GitAlternatePathLossy: return Text::tr("alternate object path cannot be inspected losslessly");
    case ProblemCode::GitAttributeInspection: return Text::tr("invalid conversion attribute inspection");
    case ProblemCode::GitPrivateObjectStore: return Text::tr("Could not create private conversion object storage");
    case ProblemCode::GitHooksResponse: return Text::tr("invalid hooks directory response");
    case ProblemCode::GitHooksPathLossy:
        return Text::tr("hooks directory cannot round-trip through Qt's directory API");
    case ProblemCode::GitHooksUninspectable: return Text::tr("hooks directory cannot be inspected");
    case ProblemCode::GitPostIndexChangeHook: return Text::tr("executable post-index-change hook is unsupported");
    case ProblemCode::GitHooksPathNotDirectory: return Text::tr("hooks path is not an inspectable directory");
    case ProblemCode::GitIndexBaseline: return Text::tr("Unsupported or oversized index baseline");
    case ProblemCode::GitIndexRecords: return Text::tr("Invalid NUL-delimited index records");
    case ProblemCode::GitStagingWithoutEntry:
        return Text::tr("Staging exited successfully without a single regular stage-zero entry");
    case ProblemCode::GitCanonicalResponse: return Text::tr("Invalid canonical object response");
    case ProblemCode::GitCanonicalSizeResponse: return Text::tr("Invalid canonical object size response");
    case ProblemCode::GitCanonicalTooLarge:
        return Text::tr("Canonical text size %1 bytes after clean conversion exceeds the %2 byte output limit");
    case ProblemCode::GitPartialOrMonitor:
        return Text::tr("unsupported partial repository or filesystem monitor setting: %1");
    case ProblemCode::GitCustomFilter: return Text::tr("custom filter '%1' is unsupported for %2");
    case ProblemCode::GitFilterAttributes: return Text::tr("invalid filter attribute inspection");
    case ProblemCode::GitFilterPath: return Text::tr("filter attributes do not match the requested path");
    case ProblemCode::GitFailedWithOutput: return Text::tr("%1");
    case ProblemCode::DiscoveryMetadataNotFile: return Text::tr("Operation metadata is not a regular file: %1");
    case ProblemCode::DiscoveryMetadataNotDirectory:
        return Text::tr("Operation metadata is not a regular directory: %1");
    case ProblemCode::DiscoveryMetadataDirectoryUnreadable:
        return Text::tr("Operation metadata directory is unreadable: %1");
    case ProblemCode::DiscoveryMetadataUnreadable: return Text::tr("Operation metadata is unreadable: %1");
    case ProblemCode::DiscoveryMetadataChangedType: return Text::tr("Operation metadata changed type: %1");
    case ProblemCode::DiscoveryMetadataCannotRead: return Text::tr("Operation metadata cannot be read: %1");
    case ProblemCode::DiscoveryMetadataOversized: return Text::tr("Operation metadata is unreadable or oversized: %1");
    case ProblemCode::DiscoveryMetadataMissing: return Text::tr("Required operation metadata is missing: %1");
    case ProblemCode::DiscoveryInvalidCommitIdentity:
        return Text::tr("Invalid commit identity in operation metadata: %1");
    case ProblemCode::DiscoveryConflictingOperations:
        return Text::tr("Conflicting merge, rebase or sequencer operation metadata");
    case ProblemCode::DiscoverySequencerUnsupported:
        return Text::tr("cherry-pick, revert and sequencer operations are unsupported");
    case ProblemCode::DiscoveryMergeAndRebaseHead: return Text::tr("MERGE_HEAD conflicts with REBASE_HEAD");
    case ProblemCode::DiscoveryMultipleMergeHeads:
        return Text::tr("merges with multiple MERGE_HEAD commits are unsupported");
    case ProblemCode::DiscoveryApplyAndRebase: return Text::tr("Conflicting apply-mailbox and rebase metadata");
    case ProblemCode::DiscoveryApplyMailbox: return Text::tr("apply-mailbox (git am) is not a supported rebase");
    case ProblemCode::DiscoveryApplyNotRebase: return Text::tr("rebase-apply metadata does not identify a rebase");
    case ProblemCode::DiscoveryInvalidHeadName: return Text::tr("Invalid rebase head-name context");
    case ProblemCode::DiscoveryNoReplayedCommit: return Text::tr("Rebase has no verified commit being replayed");
    case ProblemCode::DiscoveryReplayDisagrees: return Text::tr("Rebase replay metadata disagrees with REBASE_HEAD");
    case ProblemCode::DiscoveryRebaseHeadAlone: return Text::tr("REBASE_HEAD exists without supported rebase metadata");
    case ProblemCode::DiscoveryUnverifiedIndex:
        return Text::tr("unresolved index entries have no verified merge or rebase identity");
    case ProblemCode::DiscoveryNoOperation: return Text::tr("No merge or rebase is in progress");
    case ProblemCode::DiscoveryInvalidCommit: return Text::tr("Invalid commit object in operation identity");
    case ProblemCode::DiscoveryInvalidTree: return Text::tr("Invalid commit tree identity");
    case ProblemCode::DiscoveryInvalidParent: return Text::tr("Invalid commit parent identity");
    case ProblemCode::DiscoveryCommitWithoutTree: return Text::tr("Operation identity has no commit tree");
    case ProblemCode::DiscoveryInvalidSourceTree: return Text::tr("Invalid NUL-delimited source tree");
    case ProblemCode::DiscoveryTreeMismatch: return Text::tr("Source tree does not match the captured conflict paths");
    case ProblemCode::DiscoveryStageProvenance:
        return Text::tr("index stage %1 provenance does not match its verified commit at %2");
    case ProblemCode::DiscoveryMissingStage:
        return Text::tr("missing index stage %1 has ambiguous source topology at %2");
    case ProblemCode::DiscoveryMetadataChanged:
        return Text::tr("Operation metadata changed during discovery; inspect again");
    case ProblemCode::DiscoveryReplayOutsideHistory:
        return Text::tr("Replayed commit is not part of the original rebase history");
    case ProblemCode::DiscoveryHeadNotOnOnto:
        return Text::tr("Current HEAD is not based on the recorded rebase onto commit");
    case ProblemCode::DiscoveryUnverifiedObject:
        return Text::tr("Git could not verify operation metadata or a required local source object");
    case ProblemCode::DiscoveryGitDirectory: return Text::tr("Git did not provide a usable per-worktree Git directory");
    case ProblemCode::DiscoveryUnmergedRecords: return Text::tr("Invalid unmerged index records during discovery");
    case ProblemCode::DiscoveryStageZero: return Text::tr("Stage zero appeared in the unmerged index");
    case ProblemCode::DiscoveryInvalidHead: return Text::tr("HEAD has no valid commit identity");
    case ProblemCode::DiscoveryMergeCommitReplay:
        return Text::tr("replaying merge commits has unsupported stage provenance");
    case ProblemCode::DiscoveryMultipleBases:
        return Text::tr("multiple merge bases have ambiguous earlier-stage provenance");
    case ProblemCode::DiscoveryInvalidBase: return Text::tr("Invalid common earlier commit identity");
    case ProblemCode::DiscoveryIndexChanged: return Text::tr("Unmerged index changed during discovery; inspect again");
    case ProblemCode::DiscoveryHeadChanged: return Text::tr("HEAD changed during discovery; inspect again");
    case ProblemCode::DiscoveryInvalidBranch: return Text::tr("Invalid local branch identity during discovery");
    case ProblemCode::ConversionUnsupportedAttributes:
        return Text::tr("unsupported or contradictory text/eol/ident attributes");
    case ProblemCode::ConversionStageEntries:
        return Text::tr("source stages require unique regular-file entries at the exact path");
    case ProblemCode::ConversionModeDisagrees: return Text::tr("stages 2 and 3 require an agreed regular-file mode");
    case ProblemCode::ConversionSourceIdentity:
        return Text::tr("selected source does not match its captured stage identity");
    case ProblemCode::ConversionBusy: return Text::tr("Conversion adapter was unexpectedly busy");
    case ProblemCode::ConversionCancelled: return Text::tr("Text conversion cancelled");
    case ProblemCode::ConversionInvalidPath: return Text::tr("invalid exact conversion path");
    case ProblemCode::ConversionMissingSnapshot:
        return Text::tr("missing or inconsistent prepared publication snapshot");
    case ProblemCode::ConversionSnapshotBytes:
        return Text::tr("prepared publication text no longer matches its serialized bytes");
    case ProblemCode::ConversionUnknownOperation: return Text::tr("Unknown text conversion operation");
    case ProblemCode::ConversionWorktreeChanged: return Text::tr("conversion worktree identity changed");
    case ProblemCode::ConversionConfigurationChanged:
        return Text::tr("conversion configuration changed while reading a source");
    case ProblemCode::ConversionPolicyChangedReload:
        return Text::tr("conversion attributes/configuration changed; reload before publishing");
    case ProblemCode::ConversionChangesBytes:
        return Text::tr("clean/checkout changes the reviewed worktree bytes, including EOL, BOM or ident expansion");
    case ProblemCode::ConversionNoRoundTrip:
        return Text::tr("source clean conversion does not round-trip to the captured canonical object");
    case ProblemCode::ConversionStagedIdentity:
        return Text::tr("staged canonical identity or executable mode differs from the reviewed result");
    case ProblemCode::ConversionStagedBytes: return Text::tr("staged canonical bytes differ from the reviewed result");
    case ProblemCode::ConversionStagedCheckout:
        return Text::tr("staged checkout or conversion policy differs from the reviewed result");
    case ProblemCode::ConversionPolicyChanged: return Text::tr("conversion policy changed during verification");
    case ProblemCode::ClassificationCancelled: return Text::tr("conflict inspection cancelled");
    case ProblemCode::ClassificationBusy: return Text::tr("conflict discovery was unexpectedly busy");
    case ProblemCode::ClassificationChanged:
        return Text::tr("operation or index changed during classification; inspect again");
    case ProblemCode::ClassificationResultChanged:
        return Text::tr("current result changed or became unreadable during classification; inspect again");
    case ProblemCode::ClassificationConfigurationChanged:
        return Text::tr("conversion configuration changed during classification; inspect again");
    case ProblemCode::ClassificationInspectionFailed: return Text::tr("repository inspection failed: %1");
    case ProblemCode::ClassificationInvalidStages: return Text::tr("invalid or duplicate conflict stages");
    case ProblemCode::ClassificationNonRegular:
        return Text::tr("symlink, submodule or other non-regular source type requires external resolution");
    case ProblemCode::ClassificationMissingSide:
        return Text::tr("missing stage 2 or 3 requires a deletion or destination decision");
    case ProblemCode::ClassificationModeConflict: return Text::tr("stages 2 and 3 disagree on the executable mode");
    case ProblemCode::ClassificationTopology:
        return Text::tr("conflicting file and directory paths require a topology decision");
    case ProblemCode::ClassificationUnsupportedOperation: return Text::tr("%1");
    case ProblemCode::ClassificationConversionBusy: return Text::tr("source conversion was unexpectedly busy");
    case ProblemCode::ClassificationStageTooLarge:
        return Text::tr(
            "Load error: stage %1 is larger than the %2 byte Git read limit; the captured file was not loaded");
    case ProblemCode::ClassificationStageNotEditable: return Text::tr("stage %1 cannot be edited: %2");
    case ProblemCode::ClassificationPoliciesChanged:
        return Text::tr("source conversion policies changed during classification");
    case ProblemCode::ClassificationResultTopology:
        return Text::tr("current result must be a regular file without symlink traversal");
    case ProblemCode::ClassificationResultUnreadable:
        return Text::tr("Load error: current result could not be read completely and stably within the %1 byte limit; "
                        "check the file and reopen the session");
    case ProblemCode::ClassificationLineTooLong:
        return Text::tr(
            "Load error: Line of %1 bytes exceeds the supported %2 byte line limit; the captured file was not loaded");
    case ProblemCode::ClassificationTextTooLarge:
        return Text::tr("Load error: Text of %1 bytes exceeds the supported %2 byte recovery payload limit; the "
                        "captured file was not loaded");
    case ProblemCode::RecoveryStagingNotRetained:
        return Text::tr("Staging evidence matched, but recovery could not retain its completion: %1");
    case ProblemCode::RecoveryNoCaptures:
        return Text::tr("Recovery has no admitted source and result captures; file remains outstanding.");
    case ProblemCode::RecoveryIdentityMismatch:
        return Text::tr("Recovered file identity does not match its exact path.");
    case ProblemCode::RecoveryFileUnvalidated:
        return Text::tr("Recovery could not validate the current repository file: %1");
    case ProblemCode::RecoveryIndexUninspectable: return Text::tr("Recovery could not inspect the current Git index.");
    case ProblemCode::RecoveryInterruptedPresent:
        return Text::tr("Interrupted %1: result bytes are present; completion is uncertain. Inspect disk and index "
                        "before retrying.");
    case ProblemCode::RecoveryInterruptedUnknown:
        return Text::tr("Interrupted %1: current disk/index evidence cannot establish completion. Outcome uncertain; "
                        "no retry was performed.");
    case ProblemCode::RecoveryChangedDuringValidation:
        return Text::tr("Disk/index changed during recovery validation; work remains outstanding.");
    case ProblemCode::RecoveryChanged:
        return Text::tr("Recovered draft retained, but the repository file, index or conversion policy changed. Work "
                        "remains outstanding; no publication is authorized.");
    case ProblemCode::RecoveryChangedAfterValidation:
        return Text::tr("Recovered draft retained, but the repository file, index or conversion policy changed after "
                        "recovery validation. Work remains outstanding; no publication is authorized.");
    case ProblemCode::RecoveryCopiedLaunch:
        return Text::tr("Recovered draft retained, but the repository file was replaced by a copy with the same bytes. "
                        "Work remains outstanding unless git mergetool is waiting on this path with those bytes.");
    case ProblemCode::RecoveryUnsafeDirectory: return Text::tr("Unsafe recovery directory rejected: %1");
    case ProblemCode::RecoverySessionActive:
        return Text::tr("Recovery session %1 is active or unavailable; it was not restored.");
    case ProblemCode::RecoverySessionExpired:
        return Text::tr("Recovery session %1 was set aside more than %2 days ago and was deleted.");
    case ProblemCode::RecoverySessionRejected: return Text::tr("Recovery %1 rejected: %2");
    case ProblemCode::RecoverySessionIdentity:
        return Text::tr("Recovery %1 rejected: worktree/operation session identity does not match.");
    case ProblemCode::RecoverySessionOtherOperation:
        return Text::tr("Recovery session %1 does not match this operation; it was not restored.");
    case ProblemCode::RecoveryMarkerNotRemoved:
        return Text::tr("Recovery session %1 is set aside and its marker could not be removed; it was not restored.");
    case ProblemCode::RecoveryChoiceLost:
        return Text::tr(
            "Recovery session %1 was not restored, and no other recovery session with app edits is restored in its "
            "place; restoration is blocked rather than guessing. To restore one, choose it in this window, or close "
            "every Branch Truce window and start Branch Truce with --recovery-session followed by the session to keep. "
            "The other sessions are set aside, not deleted.");
    case ProblemCode::RecoveryChoicesNeeded:
        return Text::tr("Multiple recovery sessions with app edits match this operation; restoration is blocked rather "
                        "than guessing. To restore one, choose it in this window, or close every Branch Truce window "
                        "and start Branch Truce with --recovery-session followed by the session to keep. The other "
                        "sessions are set aside, not deleted.");
    case ProblemCode::RecoverySessionLastChanged: return Text::tr("Recovery session %1 was last changed %2.");
    case ProblemCode::RecoverySetAsideFailed: return Text::tr("Recovery session %1 could not be set aside: %2");
    case ProblemCode::RecoverySetAside:
        return Text::tr("Recovery session %1 was set aside, not deleted; it is kept for %2 days.");
    case ProblemCode::RecoveryDeletedWithoutEdits:
        return Text::tr("Recovery sessions without app edits that were deleted: %1.");
    case ProblemCode::RecoveryTailIgnored:
        return Text::tr("An interrupted final recovery record was ignored. Pending input may need repeating.");
    case ProblemCode::RecoveryOperationChanged:
        return Text::tr("Operation identity changed during recovery validation; work remains outstanding.");
    case ProblemCode::RecoveryMarkerPermissions:
        return Text::tr("Could not restrict the set-aside marker permissions.");
    case ProblemCode::MergetoolPathEmpty: return Text::tr("The mergetool path is empty.");
    case ProblemCode::MergetoolPathLeaves: return Text::tr("The mergetool path leaves its directory.");
    case ProblemCode::MergetoolPathUnopenable: return Text::tr("The mergetool path could not be opened.");
    case ProblemCode::MergetoolPathNotRegular: return Text::tr("The mergetool path is not a regular file.");
    case ProblemCode::MergetoolFileUnreadable: return Text::tr("The mergetool file could not be read.");
    case ProblemCode::MergetoolFileTooLarge:
        return Text::tr("The mergetool file exceeds the %1 byte recovery payload limit.");
    case ProblemCode::MergetoolRestoreFailed: return Text::tr("The original file could not be restored.");
    case ProblemCode::MergetoolInvocationInvalid: return Text::tr("The mergetool invocation is invalid.");
    case ProblemCode::MergetoolIntakeCreate: return Text::tr("The mergetool intake could not be created.");
    case ProblemCode::MergetoolIntakeWrite: return Text::tr("The mergetool intake could not be written.");
    case ProblemCode::MergetoolIntakeOutside: return Text::tr("The mergetool intake is outside app-owned storage.");
    case ProblemCode::MergetoolIntakeIdentity:
        return Text::tr("The mergetool intake identity does not match this session.");
    case ProblemCode::MergetoolIntakeManifest: return Text::tr("The mergetool intake manifest is invalid.");
    case ProblemCode::MergetoolIntakePath: return Text::tr("The mergetool intake path is invalid.");
    case ProblemCode::MergetoolIntakeBytes: return Text::tr("The mergetool intake bytes do not match its manifest.");
    case ProblemCode::MergetoolNoSocketDirectory:
        return Text::tr("No private mergetool socket directory is available: XDG_RUNTIME_DIR is unset or too long, and "
                        "none could be made under /tmp.");
    case ProblemCode::MergetoolSocketCreate: return Text::tr("The mergetool socket directory %1 could not be created.");
    case ProblemCode::MergetoolSocketNotDirectory:
        return Text::tr("The mergetool socket directory %1 is not a directory.");
    case ProblemCode::MergetoolSocketNotPrivate:
        return Text::tr("The mergetool socket directory %1 is not private to this user.");
    case ProblemCode::MergetoolMessageInvalid: return Text::tr("The mergetool message is invalid.");
    case ProblemCode::MergetoolOtherVersion:
        return Text::tr("The mergetool message uses protocol version %1; this side speaks %2.");
    }
    return {};
}

QString fill(const QString &sentence, const QStringList &values)
{
    switch (values.size()) {
    case 0: return sentence;
    case 1: return sentence.arg(values[0]);
    case 2: return sentence.arg(values[0], values[1]);
    default: return sentence.arg(values[0], values[1], values[2]);
    }
}

// The sentence without its "Outside app: " lead.
QString body(const Problem &problem)
{
    QStringList values;
    // An argument can hold an encoded problem of its own, such as a store's error.
    for (const auto &argument : problem.arguments) values.push_back(detail(argument));
    const auto inner = !problem.cause ? QString{}
        : outsideApp(problem.code)    ? body(*problem.cause)
                                      : text(*problem.cause);
    switch (wrap(problem.code)) {
    case Wrap::None: break;
    case Wrap::Argument: return fill(format(problem.code), values << inner);
    case Wrap::After: {
        const auto own = fill(format(problem.code), values);
        return inner.isEmpty() ? own : inner + QStringLiteral("; ") + own;
    }
    case Wrap::Output: return inner.isEmpty() ? values.value(0) : inner + QStringLiteral(": ") + values.value(0);
    }
    return fill(format(problem.code), values);
}
}

QString text(const Problem &problem)
{
    const auto sentence = body(problem);
    return outsideApp(problem.code) ? Text::tr("Outside app: %1").arg(sentence) : sentence;
}

QString detail(const QString &detail)
{
    // Layers that cannot translate join an encoded problem with text of their
    // own, so every encoded problem in detail is replaced where it stands.
    const auto bytes = detail.toUtf8();
    const QByteArray start("{\"arguments\":");
    QString shown;
    qsizetype done = 0;
    for (auto at = bytes.indexOf(start); at >= 0; at = bytes.indexOf(start, at + 1)) {
        QJsonParseError error{};
        QJsonDocument::fromJson(bytes.mid(at), &error);
        const auto length = error.error == QJsonParseError::NoError ? bytes.size() - at
            : error.error == QJsonParseError::GarbageAtEnd          ? qsizetype{error.offset}
                                                                    : qsizetype{0};
        const auto problem = length > 0 ? Problem::decode(QString::fromUtf8(bytes.mid(at, length))) : std::nullopt;
        if (!problem) continue;
        shown += QString::fromUtf8(bytes.mid(done, at - done)) + text(*problem);
        done = at + length;
        at = done - 1;
    }
    return done == 0 ? detail : shown + QString::fromUtf8(bytes.mid(done));
}

QString details(const QStringList &details)
{
    QStringList lines;
    for (const auto &line : details) lines.push_back(detail(line));
    return lines.join(QLatin1Char('\n'));
}
}
