// Engine text turns the merge engine's typed statuses, failures and preview notes into translated sentences.
#include "EngineText.h"
#include "problems/ProblemText.h"

#include <QCoreApplication>
#include <QMimeDatabase>

namespace branch_truce::engine_text {
namespace {
QString utf8(const std::string &value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

QString refusal(ChoiceNote note)
{
    switch (note) {
    case ChoiceNote::StaleRevision:
        return QCoreApplication::translate("EngineText", "The result changed. Select the scope again.");
    case ChoiceNote::FileNotEditable:
        return QCoreApplication::translate("EngineText", "This file cannot be edited in Branch Truce.");
    case ChoiceNote::UnknownScope: return QCoreApplication::translate("EngineText", "Unknown source scope.");
    case ChoiceNote::UnmappedScope:
        return QCoreApplication::translate("EngineText",
                                           "This scope cannot be mapped safely. Edit the result directly.");
    case ChoiceNote::NoRecordedChoices:
        return QCoreApplication::translate("EngineText",
                                           "This draft has no recorded scoped choices. Edit the result directly.");
    case ChoiceNote::AlreadyDecided:
        return QCoreApplication::translate("EngineText",
                                           "This scope already has a decision. Undo intervening actions to change it.");
    case ChoiceNote::SeveralCompetitors:
        return QCoreApplication::translate(
            "EngineText", "Several competing scopes could match. Use a section or edit the result directly.");
    case ChoiceNote::NoCompetitor:
        return QCoreApplication::translate(
            "EngineText",
            "The two source scopes cannot be combined safely. Use a section or edit the result directly.");
    case ChoiceNote::UnplacedSectionLines:
        return QCoreApplication::translate(
            "EngineText", "Other lines in this section cannot be located safely. Use a section or edit manually.");
    case ChoiceNote::ReplacesUnselected:
        return QCoreApplication::translate(
            "EngineText", "This combination would replace an unselected contribution. Use a section or edit manually.");
    case ChoiceNote::UncertainDeletion:
        return QCoreApplication::translate("EngineText", "The deletion scope is uncertain. Edit the result directly.");
    case ChoiceNote::AcceptedOverlap:
        return QCoreApplication::translate(
            "EngineText",
            "Accepted text may overlap this choice. Preview an ordered combination or undo its acceptance.");
    case ChoiceNote::MissingCompetitor:
        return QCoreApplication::translate("EngineText", "Missing competing source scope.");
    case ChoiceNote::UncertainCompetingLine:
        return QCoreApplication::translate("EngineText",
                                           "The competing line scope is uncertain. Use a section or edit manually.");
    case ChoiceNote::NoExactCompetingLine:
        return QCoreApplication::translate("EngineText",
                                           "This line has no exact competing scope. Use a section or edit manually.");
    case ChoiceNote::CompetingAccepted:
        return QCoreApplication::translate("EngineText", "Undo the competing acceptance before resolving this scope.");
    case ChoiceNote::NoCompetingLine:
        return QCoreApplication::translate(
            "EngineText", "No competing line can be identified safely. Use a section or edit manually.");
    case ChoiceNote::Ignore:
    case ChoiceNote::Accept:
    case ChoiceNote::ResolveUsingSide:
    case ChoiceNote::Combine: break;
    }
    return {};
}

QString combination(const ChoicePreview &preview)
{
    auto text = preview.action == ChoiceAction::CombineLeftFirst
        ? QCoreApplication::translate("EngineText", "Take both: Left then Right.")
        : QCoreApplication::translate("EngineText", "Take both: Right then Left.");
    text += QLatin1Char(' ')
        + QCoreApplication::translate(
                "EngineText",
                "Only the displayed source scopes are combined. Review the exact text for duplication and edit it "
                "afterwards if needed.");
    if (preview.markers == MarkerEffect::Removed)
        text +=
            QLatin1Char(' ') + QCoreApplication::translate("EngineText", "The conflict's marker lines are removed.");
    else if (preview.markers == MarkerEffect::KeptUntilDecided)
        text += QLatin1Char(' ')
            + QCoreApplication::translate(
                    "EngineText",
                    "The conflict keeps its marker lines until its other sections are decided, then holds this text in "
                    "this order.");
    return text;
}
}

QString status(FileStatus status)
{
    switch (status) {
    case FileStatus::OutsideApp: return QCoreApplication::translate("EngineText", "Needs work outside Branch Truce");
    case FileStatus::Unreadable: return QCoreApplication::translate("EngineText", "File could not be read");
    case FileStatus::LargeFile: return QCoreApplication::translate("EngineText", "Large file not loaded");
    case FileStatus::OutcomeUncertain:
        return QCoreApplication::translate("EngineText", "Outcome uncertain; verification required");
    case FileStatus::CheckpointFailed: return QCoreApplication::translate("EngineText", "Draft save failed");
    case FileStatus::WriteFailed: return QCoreApplication::translate("EngineText", "Repository write failed");
    case FileStatus::StageFailed:
        return QCoreApplication::translate("EngineText", "Written, unresolved; Git resolution failed");
    case FileStatus::EarlierStaged:
        return QCoreApplication::translate("EngineText", "Earlier result staged; current draft not applied");
    case FileStatus::Resolved: return QCoreApplication::translate("EngineText", "Written and resolved");
    case FileStatus::WrittenUnresolved: return QCoreApplication::translate("EngineText", "Written, unresolved");
    case FileStatus::SavedDraft:
        return QCoreApplication::translate("EngineText", "Draft saved in Branch Truce; repository file unchanged");
    case FileStatus::Undecided: return QCoreApplication::translate("EngineText", "Source choices remain undecided");
    case FileStatus::ReviewNeeded: return QCoreApplication::translate("EngineText", "Current result needs review");
    }
    return {};
}

QString blocker(Blocker blocker)
{
    switch (blocker) {
    case Blocker::OutsideApp: return QCoreApplication::translate("EngineText", "Handle this file outside Branch Truce");
    case Blocker::Unreadable: return QCoreApplication::translate("EngineText", "File content unavailable");
    case Blocker::LargeFile:
        return QCoreApplication::translate("EngineText", "Large file waits for confirmation to load");
    case Blocker::UnresolvedMarkers: return QCoreApplication::translate("EngineText", "Known conflict markers remain");
    case Blocker::Undecided:
        return QCoreApplication::translate("EngineText", "Undecided source choices need confirmation");
    case Blocker::UnsavedDraft:
        return QCoreApplication::translate("EngineText", "Current draft has no manual Save checkpoint");
    case Blocker::ReviewNeeded: return QCoreApplication::translate("EngineText", "Current result needs Mark resolved");
    case Blocker::WriteRequired:
        return QCoreApplication::translate("EngineText", "Current result has not been verified written");
    case Blocker::StageRequired:
        return QCoreApplication::translate("EngineText", "Current result has not been verified staged");
    case Blocker::EarlierStagedResult:
        return QCoreApplication::translate("EngineText", "Earlier result staged; current draft not applied");
    case Blocker::CheckpointFailed:
        return QCoreApplication::translate("EngineText", "Draft save failed; retry or discard explicitly");
    case Blocker::WriteFailed: return QCoreApplication::translate("EngineText", "Repository write failed");
    case Blocker::StageFailed: return QCoreApplication::translate("EngineText", "Git resolution failed");
    case Blocker::UncertainOutcome:
        return QCoreApplication::translate("EngineText", "Inspect uncertain mutation before finishing");
    }
    return {};
}

QString textLimit(const TextAssessment &assessment)
{
    switch (assessment.limit) {
    case TextLimit::LineBytes:
        return QCoreApplication::translate("EngineText", "Line of %1 bytes exceeds the supported %2 byte line limit")
            .arg(static_cast<qulonglong>(assessment.maxLineBytes))
            .arg(static_cast<qulonglong>(kMaxLineBytes));
    case TextLimit::RecoveryPayload:
        return QCoreApplication::translate("EngineText",
                                           "Text of %1 bytes exceeds the supported %2 byte recovery payload limit")
            .arg(static_cast<qulonglong>(assessment.bytes))
            .arg(static_cast<qulonglong>(kMaxRecoveryPayloadBytes));
    case TextLimit::Within: break;
    }
    return {};
}

QString loadFailure(const LoadFailure &failure)
{
    switch (failure.reason) {
    case LoadFailureReason::TextTooLarge:
        return QCoreApplication::translate("EngineText", "Load error: %1").arg(textLimit(failure.text));
    case LoadFailureReason::InvalidText:
        return QCoreApplication::translate("EngineText", "Load error: text is not UTF-8 with LF line endings");
    case LoadFailureReason::LargeFile:
        return QCoreApplication::translate("EngineText", "File of %n lines is over %1 lines and was not loaded",
                                           nullptr, static_cast<int>(failure.lines))
            .arg(static_cast<qulonglong>(kLargeFileLines));
    case LoadFailureReason::MarkerSizeTooLarge:
        return QCoreApplication::translate("EngineText", "Outside app: conflict-marker-size is over %1")
            .arg(static_cast<qulonglong>(kMaxConflictMarkerSize));
    case LoadFailureReason::MetadataTooLarge:
        return QCoreApplication::translate(
            "EngineText", "Comparison metadata exceeds the supported limit. Resolve this file outside the app.");
    case LoadFailureReason::OutOfMemory:
        return QCoreApplication::translate("EngineText", "Load error: not enough memory to align the captured sources");
    case LoadFailureReason::AlignmentFailed:
        return QCoreApplication::translate("EngineText", "Load error: could not align the captured sources");
    }
    return {};
}

QString supportDetail(const FileSession &file)
{
    if (const auto &failure = file.loadFailure()) return loadFailure(*failure);
    const auto problem = Problem::decode(utf8(file.supportDetail()));
    return problem ? problem_text::text(*problem) : utf8(file.supportDetail());
}

bool binaryExcluded(const FileSession &file)
{
    auto problem = Problem::decode(utf8(file.supportDetail()));
    if (file.support() != Support::OutsideApp || !problem) return false;
    if (problem->code == ProblemCode::ClassificationStageNotEditable && problem->cause) problem = *problem->cause;
    if (problem->cause) return false;
    if (problem->code == ProblemCode::TextControlCharacters) return true;
    if (problem->code != ProblemCode::TextInvalidUtf8) return false;
    // Invalid UTF-8 in a text or unknown type does not prove that it is binary.
    const auto name = utf8(file.path());
    if (name.toUtf8() != QByteArray::fromStdString(file.path())) return false;
    const auto mime = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    return mime.isValid() && !mime.isDefault() && !mime.inherits(QStringLiteral("text/plain"));
}

QString choice(const ChoicePreview &preview)
{
    const bool removed = preview.markers == MarkerEffect::Removed;
    switch (preview.note) {
    case ChoiceNote::Combine: return combination(preview);
    case ChoiceNote::ResolveUsingSide:
        return removed
            ? QCoreApplication::translate(
                  "EngineText",
                  "Resolve using this side accepts this scope, ignores its competing contributions and "
                  "removes the conflict's marker lines and ignored text.")
            : QCoreApplication::translate(
                  "EngineText", "Resolve using this side accepts this scope and ignores its competing contributions.");
    case ChoiceNote::Ignore:
    case ChoiceNote::Accept:
        if (removed)
            return QCoreApplication::translate(
                "EngineText",
                "This decides the last open side of the conflict, so its marker lines and ignored text are "
                "removed.");
        return preview.note == ChoiceNote::Ignore
            ? QCoreApplication::translate("EngineText",
                                          "Ignore preserves result text and leaves the other side undecided.")
            : QCoreApplication::translate("EngineText",
                                          "Accept changes only this scope and leaves the other side undecided.");
    default: return refusal(preview.note);
    }
}

QString batch(BatchNote note)
{
    switch (note) {
    case BatchNote::Batch:
        return QCoreApplication::translate(
            "EngineText",
            "Only the displayed non-conflicting changes in this file will be applied. Competing choices, "
            "existing decisions, manual edits and uncertain passages are left alone.");
    case BatchNote::StaleRevision:
        return QCoreApplication::translate("EngineText", "The result changed. Preview the batch again.");
    case BatchNote::FileNotEditable:
        return QCoreApplication::translate("EngineText", "This file cannot apply non-conflicting changes.");
    case BatchNote::NoEarlierVersion:
        return QCoreApplication::translate(
            "EngineText",
            "The common earlier version is unavailable. Non-conflicting changes cannot be identified safely.");
    }
    return {};
}

QString wholeFile(WholeFileNote note)
{
    switch (note) {
    case WholeFileNote::Replace:
        return QCoreApplication::translate(
            "EngineText",
            "Replace the entire result for this file with the selected source. This gives up the other "
            "source's contributions and replaces all differing current text, including edits made before "
            "opening and manual edits made in this session. Inspect the full result, contribution decisions "
            "and replaced passages before confirming. Undo restores the previous text and decisions. Review "
            "the result before saving.");
    case WholeFileNote::StaleRevision:
        return QCoreApplication::translate("EngineText", "The result changed. Inspect a new whole-file preview.");
    case WholeFileNote::NoCapturedSource:
        return QCoreApplication::translate("EngineText", "This file has no editable captured source.");
    case WholeFileNote::InvalidSide:
        return QCoreApplication::translate("EngineText", "Choose Left or Right for whole-file replacement.");
    case WholeFileNote::SourceUnavailable:
        return QCoreApplication::translate("EngineText", "The selected source is unavailable.");
    }
    return {};
}

}
