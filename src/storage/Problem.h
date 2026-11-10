// A problem is a typed failure with the values its message names, which the app turns into translated text.
#pragma once

#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
#include <utility>

namespace branch_truce {

// Every failure the layers between the engine and the app report. Each name
// is stable: an encoded problem stores it. src/app/ProblemText.cpp holds the
// sentence of each; nothing below the app formats one.
// clang-format off
#define BRANCH_TRUCE_PROBLEMS(X) \
    /* Text formats */ \
    X(TextUnsupportedEncoding) \
    X(TextUnsupportedEol) \
    X(TextInvalidUtf8) \
    X(TextMissingByteOrderMark) \
    X(TextContradictoryByteOrderMark) \
    X(TextTruncatedUtf16) \
    X(TextInvalidSurrogatePair) \
    X(TextUnpairedSurrogate) \
    X(TextControlCharacters) \
    X(TextBareCarriageReturn) \
    X(TextMixedNewlines) \
    X(TextNoRoundTrip) \
    X(TextInvalidEditable) \
    X(TextSerializedNoRoundTrip) \
    /* Destination writes; each System* code names the call and carries the system's error text */ \
    X(DestinationInvalidPath) \
    X(DestinationSymlink) \
    X(DestinationDirectorySymlink) \
    X(DestinationReplacedBySymlink) \
    X(DestinationAncestorNotDirectory) \
    X(DestinationNotRegular) \
    X(DestinationSizeUnrepresentable) \
    X(DestinationDirectoryChangedWhileOpening) \
    X(DestinationChangedWhileOpening) \
    X(DestinationShrank) \
    X(DestinationChangedWhileReading) \
    X(DestinationDirectoryChanged) \
    X(DestinationIdentityChanged) \
    X(DestinationContentChanged) \
    X(DestinationChangedWhileReopening) \
    X(DestinationChangedDuringPermissionCheck) \
    X(PublicationInvalidRequest) \
    X(PublicationOwnership) \
    X(PublicationExtendedAttributes) \
    X(PublicationSelinuxLabel) \
    X(PublicationCancelled) \
    X(PublicationNoProgress) \
    X(PublicationReplacementChanged) \
    X(PublicationUnsupportedFilesystem) \
    X(PublicationUnverified) \
    X(PublicationCleanupFailed) \
    X(PublicationCleanupIncomplete) \
    X(SystemInspectDirectory) \
    X(SystemOpenDirectory) \
    X(SystemInspectOpenedDirectory) \
    X(SystemInspectDestination) \
    X(SystemReadDestination) \
    X(SystemFinishReading) \
    X(SystemReinspectDestination) \
    X(SystemReopenRoot) \
    X(SystemReinspectEntry) \
    X(SystemReopenEntry) \
    X(SystemInspectAttributes) \
    X(SystemReadLabel) \
    X(SystemInspectFile) \
    X(SystemOpenFile) \
    X(SystemReopenDestination) \
    X(SystemNotWritable) \
    X(SystemInspectWritable) \
    X(SystemCreatePrivateDirectory) \
    X(SystemCreateReplacement) \
    X(SystemWriteReplacement) \
    X(SystemPreservePermissions) \
    X(SystemSyncReplacement) \
    X(SystemExchangeUncertain) \
    X(SystemExchange) \
    X(SystemSyncPublished) \
    /* Recovery storage */ \
    X(RecoveryPayloadTooLarge) \
    X(RecoveryPayloadUnsafe) \
    X(RecoveryPayloadIntegrity) \
    X(RecoveryJournalUnsafe) \
    X(RecoveryJournalIncomplete) \
    X(RecoveryJournalNewer) \
    X(RecoveryJournalVersion) \
    X(RecoveryRecordIntegrity) \
    X(RecoveryRecordSchema) \
    X(RecoveryTransactionRepeated) \
    X(RecoveryStorageLocation) \
    X(RecoveryTransactionInvalid) \
    X(RecoveryStorageCreate) \
    X(RecoveryJournalInitialize) \
    X(RecoveryCommitBoundary) \
    X(RecoveryTransactionReused) \
    X(RecoveryPendingFlush) \
    X(RecoveryPayloadIdentity) \
    X(RecoveryPayloadCommit) \
    X(RecoveryStorageSync) \
    X(RecoveryCommitFailed) \
    /* Git requests */ \
    X(GitInvalidRequest) \
    X(GitUnknownOperation) \
    X(GitInvalidOperands) \
    X(GitInputTooLarge) \
    X(GitCleanupFailed) \
    X(GitCancelled) \
    X(GitStartRefused) \
    X(GitInputNotSent) \
    X(GitDeadline) \
    X(GitReinspectionTimedOut) \
    X(GitLaunchFailed) \
    X(GitProcessFailed) \
    X(GitStartFailed) \
    X(GitLaunchPipe) \
    X(GitExecutableChangedBeforeReinspection) \
    X(GitExecutableChanged) \
    X(GitReinspectionTooLarge) \
    X(GitOutputLimit) \
    X(GitReinspectionFailed) \
    X(GitExitCode) \
    X(GitCrashed) \
    X(GitNotGit) \
    X(GitWorktreeRootResponse) \
    X(GitExecutableInRepository) \
    X(GitStagingNotRegular) \
    X(GitObjectDirectoryResponse) \
    X(GitObjectStoreUninspectable) \
    X(GitTooManyAlternates) \
    X(GitPacksUninspectable) \
    X(GitPromisorPacks) \
    X(GitAlternatesUninspectable) \
    X(GitAlternatesUnreadable) \
    X(GitAlternatePathLossy) \
    X(GitAttributeInspection) \
    X(GitPrivateObjectStore) \
    X(GitHooksResponse) \
    X(GitHooksPathLossy) \
    X(GitHooksUninspectable) \
    X(GitPostIndexChangeHook) \
    X(GitHooksPathNotDirectory) \
    X(GitIndexBaseline) \
    X(GitIndexRecords) \
    X(GitStagingWithoutEntry) \
    X(GitCanonicalResponse) \
    X(GitCanonicalSizeResponse) \
    X(GitCanonicalTooLarge) \
    X(GitPartialOrMonitor) \
    X(GitCustomFilter) \
    X(GitFilterAttributes) \
    X(GitFilterPath) \
    X(GitFailedWithOutput) \
    /* Repository discovery */ \
    X(DiscoveryMetadataNotFile) \
    X(DiscoveryMetadataNotDirectory) \
    X(DiscoveryMetadataDirectoryUnreadable) \
    X(DiscoveryMetadataUnreadable) \
    X(DiscoveryMetadataChangedType) \
    X(DiscoveryMetadataCannotRead) \
    X(DiscoveryMetadataOversized) \
    X(DiscoveryMetadataMissing) \
    X(DiscoveryInvalidCommitIdentity) \
    X(DiscoveryConflictingOperations) \
    X(DiscoverySequencerUnsupported) \
    X(DiscoveryMergeAndRebaseHead) \
    X(DiscoveryMultipleMergeHeads) \
    X(DiscoveryApplyAndRebase) \
    X(DiscoveryApplyMailbox) \
    X(DiscoveryApplyNotRebase) \
    X(DiscoveryInvalidHeadName) \
    X(DiscoveryNoReplayedCommit) \
    X(DiscoveryReplayDisagrees) \
    X(DiscoveryRebaseHeadAlone) \
    X(DiscoveryUnverifiedIndex) \
    X(DiscoveryNoOperation) \
    X(DiscoveryInvalidCommit) \
    X(DiscoveryInvalidTree) \
    X(DiscoveryInvalidParent) \
    X(DiscoveryCommitWithoutTree) \
    X(DiscoveryInvalidSourceTree) \
    X(DiscoveryTreeMismatch) \
    X(DiscoveryStageProvenance) \
    X(DiscoveryMissingStage) \
    X(DiscoveryMetadataChanged) \
    X(DiscoveryReplayOutsideHistory) \
    X(DiscoveryHeadNotOnOnto) \
    X(DiscoveryUnverifiedObject) \
    X(DiscoveryGitDirectory) \
    X(DiscoveryUnmergedRecords) \
    X(DiscoveryStageZero) \
    X(DiscoveryInvalidHead) \
    X(DiscoveryMergeCommitReplay) \
    X(DiscoveryMultipleBases) \
    X(DiscoveryInvalidBase) \
    X(DiscoveryIndexChanged) \
    X(DiscoveryHeadChanged) \
    X(DiscoveryInvalidBranch) \
    /* Text conversion */ \
    X(ConversionUnsupportedAttributes) \
    X(ConversionStageEntries) \
    X(ConversionModeDisagrees) \
    X(ConversionSourceIdentity) \
    X(ConversionBusy) \
    X(ConversionCancelled) \
    X(ConversionInvalidPath) \
    X(ConversionMissingSnapshot) \
    X(ConversionSnapshotBytes) \
    X(ConversionUnknownOperation) \
    X(ConversionWorktreeChanged) \
    X(ConversionConfigurationChanged) \
    X(ConversionPolicyChangedReload) \
    X(ConversionChangesBytes) \
    X(ConversionNoRoundTrip) \
    X(ConversionStagedIdentity) \
    X(ConversionStagedBytes) \
    X(ConversionStagedCheckout) \
    X(ConversionPolicyChanged) \
    /* Conflict classification */ \
    X(ClassificationCancelled) \
    X(ClassificationBusy) \
    X(ClassificationChanged) \
    X(ClassificationResultChanged) \
    X(ClassificationConfigurationChanged) \
    X(ClassificationInspectionFailed) \
    X(ClassificationInvalidStages) \
    X(ClassificationNonRegular) \
    X(ClassificationMissingSide) \
    X(ClassificationModeConflict) \
    X(ClassificationTopology) \
    X(ClassificationUnsupportedOperation) \
    X(ClassificationConversionBusy) \
    X(ClassificationStageTooLarge) \
    X(ClassificationStageNotEditable) \
    X(ClassificationPoliciesChanged) \
    X(ClassificationResultTopology) \
    X(ClassificationResultUnreadable) \
    X(ClassificationLineTooLong) \
    X(ClassificationTextTooLarge) \
    /* Recovery */ \
    X(RecoveryStagingNotRetained) \
    X(RecoveryNoCaptures) \
    X(RecoveryIdentityMismatch) \
    X(RecoveryFileUnvalidated) \
    X(RecoveryIndexUninspectable) \
    X(RecoveryInterruptedPresent) \
    X(RecoveryInterruptedUnknown) \
    X(RecoveryChangedDuringValidation) \
    X(RecoveryChanged) \
    X(RecoveryChangedAfterValidation) \
    X(RecoveryCopiedLaunch) \
    X(RecoveryUnsafeDirectory) \
    X(RecoverySessionActive) \
    X(RecoverySessionExpired) \
    X(RecoverySessionRejected) \
    X(RecoverySessionIdentity) \
    X(RecoverySessionOtherOperation) \
    X(RecoveryMarkerNotRemoved) \
    X(RecoveryChoiceLost) \
    X(RecoveryChoicesNeeded) \
    X(RecoverySessionLastChanged) \
    X(RecoverySetAsideFailed) \
    X(RecoverySetAside) \
    X(RecoveryDeletedWithoutEdits) \
    X(RecoveryTailIgnored) \
    X(RecoveryOperationChanged) \
    X(RecoveryMarkerPermissions) \
    /* Mergetool files, intakes and sockets */ \
    X(MergetoolPathEmpty) \
    X(MergetoolPathLeaves) \
    X(MergetoolPathUnopenable) \
    X(MergetoolPathNotRegular) \
    X(MergetoolFileUnreadable) \
    X(MergetoolFileTooLarge) \
    X(MergetoolRestoreFailed) \
    X(MergetoolInvocationInvalid) \
    X(MergetoolIntakeCreate) \
    X(MergetoolIntakeWrite) \
    X(MergetoolIntakeOutside) \
    X(MergetoolIntakeIdentity) \
    X(MergetoolIntakeManifest) \
    X(MergetoolIntakePath) \
    X(MergetoolIntakeBytes) \
    X(MergetoolNoSocketDirectory) \
    X(MergetoolSocketCreate) \
    X(MergetoolSocketNotDirectory) \
    X(MergetoolSocketNotPrivate) \
    X(MergetoolMessageInvalid) \
    X(MergetoolOtherVersion)
// clang-format on

enum class ProblemCode {
    None,
#define BRANCH_TRUCE_PROBLEM_ENUMERATOR(name) name,
    BRANCH_TRUCE_PROBLEMS(BRANCH_TRUCE_PROBLEM_ENUMERATOR)
#undef BRANCH_TRUCE_PROBLEM_ENUMERATOR
};

// One problem: its code, the values its message names in order, and the
// problem it wraps, if any. Arguments are display values such as paths,
// counts and the system's own error text, never sentences of this app.
struct Problem {
    ProblemCode code = ProblemCode::None;
    QStringList arguments = {};
    std::shared_ptr<const Problem> cause = {};

    Problem() = default;
    Problem(ProblemCode problemCode, QStringList values = {}) : code(problemCode), arguments(std::move(values)) {}
    // This problem, wrapping inner.
    [[nodiscard]] Problem wrapping(Problem inner) const;
    [[nodiscard]] bool isEmpty() const { return code == ProblemCode::None; }
    [[nodiscard]] bool operator==(const Problem &other) const;
    // Whether this problem or one it wraps has code.
    [[nodiscard]] bool involves(ProblemCode problemCode) const;

    // A stable text form for strings that cross into the engine, the session
    // store or a journal, such as a file's support detail. The app decodes
    // and translates it where it shows it.
    [[nodiscard]] QString encoded() const;
    // The problem an encoded text holds, or nothing for any other text.
    [[nodiscard]] static std::optional<Problem> decode(const QString &text);
};

// The stable name of a code, as encoded problems store it.
[[nodiscard]] QString problemName(ProblemCode code);
} // namespace branch_truce
