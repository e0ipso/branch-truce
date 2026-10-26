#include "SessionRecovery.h"
#include <QJsonArray>

namespace branch_truce {
namespace {
class Encoder {
public:
    storage::RecoveryTransaction transaction;

    QString bytes(const std::string &value)
    {
        const QByteArray data(value.data(), static_cast<qsizetype>(value.size()));
        const auto hash = storage::RecoveryStore::digest(data);
        transaction.payloads.insert(hash, data);
        return QString::fromLatin1(hash);
    }
    QJsonObject draft(const Draft &value)
    {
        QJsonArray decisions;
        for (const auto &[id, decision] : value.decisions) {
            QJsonObject entry{{QStringLiteral("id"), bytes(id)},
                {QStringLiteral("value"), static_cast<int>(decision.value)}};
            if (decision.order) entry.insert(QStringLiteral("order"), QString::number(*decision.order));
            decisions.push_back(entry);
        }
        return {{QStringLiteral("text"), bytes(value.text)}, {QStringLiteral("decisions"), decisions},
            {QStringLiteral("unresolvedMarkers"), QString::number(value.unresolvedMarkers)}};
    }
    QJsonObject revision(const Revision &value)
    {
        QJsonObject result{{QStringLiteral("id"), QString::number(value.id)},
            {QStringLiteral("result"), QString::number(value.result)},
            {QStringLiteral("decisions"), QString::number(value.decisions)},
            {QStringLiteral("draft"), draft(value.draft)}};
        // Written only when present, so records without hand edits keep the
        // encoding they had before this field existed.
        QJsonArray handEdited;
        if (value.correspondence)
            for (const auto &id : value.correspondence->handEditedAnchors()) handEdited.push_back(bytes(id));
        if (!handEdited.isEmpty()) result.insert(QStringLiteral("handEdited"), handEdited);
        return result;
    }
    QJsonValue outcome(const std::optional<OperationOutcome> &value)
    {
        if (!value) return QJsonValue::Null;
        return QJsonObject{{QStringLiteral("revision"), QString::number(value->revision)},
            {QStringLiteral("outcome"), static_cast<int>(value->outcome)},
            {QStringLiteral("detail"), bytes(value->detail)}};
    }
    QJsonObject text(const TextSnapshot &value)
    {
        return {{QStringLiteral("utf8"), bytes(value.utf8)}, {QStringLiteral("bytes"), bytes(value.bytes)},
            {QStringLiteral("format"), QJsonObject{
                {QStringLiteral("encoding"), static_cast<int>(value.format.encoding)},
                {QStringLiteral("bom"), value.format.bom},
                {QStringLiteral("newline"), static_cast<int>(value.format.newline)},
                {QStringLiteral("finalNewline"), value.format.finalNewline}}}};
    }
    QJsonValue source(const std::optional<SourceSnapshot> &value)
    {
        if (!value) return QJsonValue::Null;
        return QJsonObject{{QStringLiteral("objectId"), bytes(value->objectId)},
            {QStringLiteral("mode"), bytes(value->mode)}, {QStringLiteral("rawBytes"), bytes(value->rawBytes)},
            {QStringLiteral("worktree"), text(value->worktree)},
            {QStringLiteral("commitId"), value->commitId ? QJsonValue(bytes(*value->commitId)) : QJsonValue::Null},
            {QStringLiteral("label"), bytes(value->label)}};
    }
    QJsonArray mapping(const std::map<std::string, std::string> &values)
    {
        QJsonArray result;
        for (const auto &[key, value] : values)
            result.push_back(QJsonObject{{QStringLiteral("key"), bytes(key)}, {QStringLiteral("value"), bytes(value)}});
        return result;
    }
    QJsonValue snapshots(const FileSnapshots *value)
    {
        if (!value) return QJsonValue::Null;
        const auto &p = value->provenance;
        const auto &i = value->launchResult.identity;
        return QJsonObject{{QStringLiteral("path"), bytes(value->path)},
            {QStringLiteral("resultMode"), bytes(value->resultMode)},
            {QStringLiteral("earlier"), source(value->earlier)},
            {QStringLiteral("stage2"), source(value->stage2)}, {QStringLiteral("stage3"), source(value->stage3)},
            {QStringLiteral("conversion"), QJsonObject{{QStringLiteral("attributes"), mapping(value->conversion.attributes)},
                {QStringLiteral("configuration"), mapping(value->conversion.configuration)}}},
            {QStringLiteral("provenance"), QJsonObject{{QStringLiteral("sessionId"), bytes(p.sessionId)},
                {QStringLiteral("worktreeRoot"), bytes(p.worktreeRoot)}, {QStringLiteral("gitDirectory"), bytes(p.gitDirectory)},
                {QStringLiteral("operation"), static_cast<int>(p.operation)},
                {QStringLiteral("stage2CommitId"), bytes(p.stage2CommitId)},
                {QStringLiteral("stage3CommitId"), bytes(p.stage3CommitId)},
                {QStringLiteral("baseCommitId"), p.baseCommitId ? QJsonValue(bytes(*p.baseCommitId)) : QJsonValue::Null},
                {QStringLiteral("headName"), bytes(p.headName)}, {QStringLiteral("originalHead"), bytes(p.originalHead)},
                {QStringLiteral("onto"), bytes(p.onto)}}},
            {QStringLiteral("launchResult"), QJsonObject{{QStringLiteral("text"), text(value->launchResult.text)},
                {QStringLiteral("identity"), QJsonObject{
                    {QStringLiteral("device"), QString::number(i.device)}, {QStringLiteral("inode"), QString::number(i.inode)},
                    {QStringLiteral("mode"), QString::number(i.mode)}, {QStringLiteral("size"), QString::number(i.size)},
                    {QStringLiteral("modifiedSeconds"), QString::number(i.modifiedSeconds)},
                    {QStringLiteral("modifiedNanoseconds"), QString::number(i.modifiedNanoseconds)},
                    {QStringLiteral("changedSeconds"), QString::number(i.changedSeconds)},
                    {QStringLiteral("changedNanoseconds"), QString::number(i.changedNanoseconds)}}}}}};
    }
};
}

storage::RecoveryTransaction recoveryTransaction(const FileSession &file,
    const QString &action, const QString &transactionId)
{
    Encoder encoder;
    auto &state = encoder.transaction.state;
    state = {{QStringLiteral("schema"), 1}, {QStringLiteral("transactionId"), transactionId},
        {QStringLiteral("action"), action}, {QStringLiteral("fileId"), encoder.bytes(file.id())},
        {QStringLiteral("path"), encoder.bytes(file.path())},
        {QStringLiteral("support"), static_cast<int>(file.support())},
        {QStringLiteral("supportDetail"), encoder.bytes(file.supportDetail())},
        {QStringLiteral("snapshots"), encoder.snapshots(file.snapshots())},
        {QStringLiteral("current"), encoder.revision(file.current())},
        {QStringLiteral("checkpoint"), file.checkpoint() ? QJsonValue(encoder.revision(*file.checkpoint())) : QJsonValue::Null},
        {QStringLiteral("written"), file.written() ? QJsonValue(encoder.revision(*file.written())) : QJsonValue::Null},
        {QStringLiteral("checkpointOutcome"), encoder.outcome(file.checkpointOutcome())},
        {QStringLiteral("writeOutcome"), encoder.outcome(file.writeOutcome())},
        {QStringLiteral("stageOutcome"), encoder.outcome(file.stageOutcome())}};
    if (file.reviewed()) {
        auto reviewed = encoder.revision(file.reviewed()->revision);
        reviewed.insert(QStringLiteral("confirmedUndecided"), file.reviewed()->confirmedUndecided);
        state.insert(QStringLiteral("reviewed"), reviewed);
    } else state.insert(QStringLiteral("reviewed"), QJsonValue::Null);
    if (file.staged()) {
        auto staged = encoder.revision(file.staged()->revision);
        staged.insert(QStringLiteral("objectId"), encoder.bytes(file.staged()->objectId));
        staged.insert(QStringLiteral("mode"), encoder.bytes(file.staged()->mode));
        staged.insert(QStringLiteral("confirmedUndecided"), file.staged()->confirmedUndecided);
        state.insert(QStringLiteral("staged"), staged);
    } else state.insert(QStringLiteral("staged"), QJsonValue::Null);
    return std::move(encoder.transaction);
}
}
