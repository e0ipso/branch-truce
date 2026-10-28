#include "ConflictCapture.h"

namespace branch_truce {
Support supportFor(const git::Conflict &file)
{
    return file.support == git::ConflictSupport::LoadFailure || file.support == git::ConflictSupport::InspectionFailure
        ? Support::Unreadable : Support::OutsideApp;
}

TextSnapshot textSnapshot(const storage::TextDocument &document)
{
    SnapshotFormat format;
    switch (document.format.encoding) {
    case storage::TextEncoding::Utf8: format.encoding = SnapshotFormat::Encoding::Utf8; break;
    case storage::TextEncoding::Utf16LittleEndian: format.encoding = SnapshotFormat::Encoding::Utf16LittleEndian; break;
    case storage::TextEncoding::Utf16BigEndian: format.encoding = SnapshotFormat::Encoding::Utf16BigEndian; break;
    }
    format.bom = document.format.bom;
    format.newline = document.format.newline == storage::Newline::CrLf
        ? SnapshotFormat::Newline::CrLf : SnapshotFormat::Newline::Lf;
    format.finalNewline = document.format.finalNewline;
    return {document.utf8.toStdString(), document.originalBytes.toStdString(), format};
}

std::optional<FileSnapshots> captureFile(const git::Conflict &file,
    const git::RepositoryContext &repository, const std::string &sessionId)
{
    if (!file.editable() || !file.result || !file.resultIdentity || !repository.supported
        || file.sources.size() != file.stages.size() || file.sources.isEmpty()) return std::nullopt;
    FileSnapshots captured;
    captured.path = file.path.toStdString();
    captured.resultMode = file.mode.toStdString();
    captured.launchResult = {textSnapshot(*file.result), *file.resultIdentity};
    auto &provenance = captured.provenance;
    provenance.sessionId = sessionId;
    provenance.worktreeRoot = repository.worktreeRoot.toStdString();
    provenance.gitDirectory = repository.gitDirectory.toStdString();
    if (repository.operation == git::RepositoryOperation::Merge) provenance.operation = SnapshotProvenance::Operation::Merge;
    else if (repository.operation == git::RepositoryOperation::Rebase) provenance.operation = SnapshotProvenance::Operation::Rebase;
    else return std::nullopt;
    provenance.stage2CommitId = repository.stage2.commitId.toStdString();
    provenance.stage3CommitId = repository.stage3.commitId.toStdString();
    if (!repository.baseCommitId.isEmpty()) provenance.baseCommitId = repository.baseCommitId.toStdString();
    provenance.headName = repository.headName.toStdString();
    provenance.originalHead = repository.originalHead.toStdString();
    provenance.onto = repository.onto.toStdString();
    const auto &policy = file.sources.front().policy;
    for (auto entry = policy.attributes.cbegin(); entry != policy.attributes.cend(); ++entry)
        captured.conversion.attributes.emplace(entry.key().toStdString(), entry.value().toStdString());
    for (auto entry = policy.configuration.cbegin(); entry != policy.configuration.cend(); ++entry)
        captured.conversion.configuration.emplace(entry.key().toStdString(), entry.value().toStdString());
    for (qsizetype i = 0; i < file.stages.size(); ++i) {
        const auto &stage = file.stages[i];
        const auto &source = file.sources[i];
        if (stage.path != file.path || source.path != file.path || source.worktreeRoot != repository.worktreeRoot
            || source.canonicalObjectId != stage.objectId || source.sourceMode != stage.mode
            || source.mode != file.mode || source.policy != policy) return std::nullopt;
        SourceSnapshot snapshot{stage.objectId.toStdString(), stage.mode.toStdString(),
            source.canonicalBytes.toStdString(), textSnapshot(source.document), std::nullopt, {}};
        if (stage.stage == 1 && !captured.earlier) {
            snapshot.commitId = provenance.baseCommitId;
            snapshot.label = "Common earlier version";
            captured.earlier = std::move(snapshot);
        } else if (stage.stage == 2 && !captured.stage2) {
            snapshot.commitId = provenance.stage2CommitId;
            snapshot.label = repository.stage2.label.toStdString();
            captured.stage2 = std::move(snapshot);
        } else if (stage.stage == 3 && !captured.stage3) {
            snapshot.commitId = provenance.stage3CommitId;
            snapshot.label = repository.stage3.label.toStdString();
            captured.stage3 = std::move(snapshot);
        } else return std::nullopt;
    }
    if (!captured.stage2 || !captured.stage3) return std::nullopt;
    return captured;
}
}
