#include "RecoveryRecord.h"
#include "storage/TextFormat.h"
#include <QJsonArray>
#include <QUuid>
#include <limits>

namespace branch_truce {
namespace {
void require(bool condition, const char *detail)
{
    if (!condition) throw std::runtime_error(detail);
}

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

std::string RecoveryRecordDecoder::bytes(const QJsonValue &value) const
{
    require(value.isString(), "Missing recovery payload reference.");
    QString error;
    const auto result = store_.payload(value.toString().toLatin1(), &error);
    require(error.isEmpty(), "Recovery payload is missing, unsafe or corrupt.");
    return result.toStdString();
}

quint64 RecoveryRecordDecoder::number(const QJsonValue &value) const
{
    bool ok = false;
    const auto result = value.toString().toULongLong(&ok);
    require(ok && QString::number(result) == value.toString(), "Invalid recovery integer.");
    return result;
}

qint64 RecoveryRecordDecoder::signedNumber(const QJsonValue &value) const
{
    bool ok = false;
    const auto result = value.toString().toLongLong(&ok);
    require(ok && QString::number(result) == value.toString(), "Invalid recovery signed integer.");
    return result;
}

bool RecoveryRecordDecoder::boolean(const QJsonValue &value) const
{
    require(value.isBool(), "Invalid recovery boolean.");
    return value.toBool();
}

Draft RecoveryRecordDecoder::draft(const QJsonValue &value) const
{
    const auto object = value.toObject();
    // The recorded marker count is advisory. FileSession scans the text
    // again, and replay keeps that count in place of the recorded one.
    (void)number(object.value("unresolvedMarkers"));
    Draft result{bytes(object.value("text")), {}, 0};
    require(validUtf8(result.text) && object.value("decisions").isArray(), "Invalid recovered draft.");
    for (const auto &entry : object.value("decisions").toArray()) {
        const auto decision = entry.toObject();
        const auto id = bytes(decision.value("id"));
        Decision state{enumeration<DecisionValue>(decision.value("value"), 2), {}};
        if (decision.contains("order")) state.order = number(decision.value("order"));
        require(result.decisions.emplace(id, state).second, "Duplicate recovered decision.");
    }
    return result;
}

std::set<std::string> RecoveryRecordDecoder::handEdited(const QJsonValue &value) const
{
    std::set<std::string> result;
    if (value.isUndefined()) return result;
    require(value.isArray(), "Invalid recovered hand edits.");
    for (const auto &entry : value.toArray())
        require(result.insert(bytes(entry)).second, "Duplicate recovered hand edit.");
    return result;
}

TextSnapshot RecoveryRecordDecoder::text(const QJsonValue &value) const
{
    const auto object = value.toObject();
    const auto format = object.value("format").toObject();
    TextSnapshot result{bytes(object.value("utf8")), bytes(object.value("bytes")),
        {enumeration<SnapshotFormat::Encoding>(format.value("encoding"), 2),
         boolean(format.value("bom")), enumeration<SnapshotFormat::Newline>(format.value("newline"), 1),
         boolean(format.value("finalNewline"))}};
    require(validUtf8(result.utf8), "Invalid recovered text encoding.");
    const auto encoding = result.format.encoding == SnapshotFormat::Encoding::Utf8 ? QByteArray{}
        : result.format.encoding == SnapshotFormat::Encoding::Utf16LittleEndian ? QByteArray("UTF-16LE") : QByteArray("UTF-16BE");
    const auto decoded = storage::decodeText(QByteArray::fromStdString(result.bytes), encoding);
    require(decoded.document && decoded.document->utf8.toStdString() == result.utf8,
            "Recovered text bytes and editable representation disagree.");
    require(static_cast<int>(decoded.document->format.encoding) == static_cast<int>(result.format.encoding)
        && decoded.document->format.bom == result.format.bom
        && static_cast<int>(decoded.document->format.newline) == static_cast<int>(result.format.newline)
        && decoded.document->format.finalNewline == result.format.finalNewline,
        "Recovered text format does not describe its bytes.");
    return result;
}

std::optional<SourceSnapshot> RecoveryRecordDecoder::source(const QJsonValue &value) const
{
    if (value.isNull()) return {};
    const auto object = value.toObject();
    SourceSnapshot result{bytes(object.value("objectId")), bytes(object.value("mode")),
        bytes(object.value("rawBytes")), text(object.value("worktree")), {}, bytes(object.value("label"))};
    if (!object.value("commitId").isNull()) result.commitId = bytes(object.value("commitId"));
    require(!result.objectId.empty() && (result.mode == "100644" || result.mode == "100755"),
            "Invalid recovered source identity.");
    return result;
}

std::map<std::string, std::string> RecoveryRecordDecoder::mapping(const QJsonValue &value) const
{
    require(value.isArray(), "Invalid recovery conversion policy.");
    std::map<std::string, std::string> result;
    for (const auto &entry : value.toArray()) {
        const auto object = entry.toObject();
        require(result.emplace(bytes(object.value("key")), bytes(object.value("value"))).second,
                "Duplicate recovery conversion policy entry.");
    }
    return result;
}

std::optional<FileSnapshots> RecoveryRecordDecoder::snapshots(const QJsonValue &value) const
{
    if (value.isNull()) return {};
    const auto object = value.toObject();
    const auto p = object.value("provenance").toObject();
    const auto launch = object.value("launchResult").toObject();
    const auto identity = launch.value("identity").toObject();
    FileSnapshots result;
    result.path = bytes(object.value("path"));
    result.resultMode = bytes(object.value("resultMode"));
    result.earlier = source(object.value("earlier"));
    result.stage2 = source(object.value("stage2"));
    result.stage3 = source(object.value("stage3"));
    const auto policy = object.value("conversion").toObject();
    result.conversion = {mapping(policy.value("attributes")), mapping(policy.value("configuration"))};
    auto &provenance = result.provenance;
    provenance.sessionId = bytes(p.value("sessionId"));
    provenance.worktreeRoot = bytes(p.value("worktreeRoot"));
    provenance.gitDirectory = bytes(p.value("gitDirectory"));
    provenance.operation = enumeration<SnapshotProvenance::Operation>(p.value("operation"), 1);
    provenance.stage2CommitId = bytes(p.value("stage2CommitId"));
    provenance.stage3CommitId = bytes(p.value("stage3CommitId"));
    if (!p.value("baseCommitId").isNull()) provenance.baseCommitId = bytes(p.value("baseCommitId"));
    provenance.headName = bytes(p.value("headName"));
    provenance.originalHead = bytes(p.value("originalHead"));
    provenance.onto = bytes(p.value("onto"));
    require(!QUuid(QString::fromStdString(provenance.sessionId)).isNull(), "Invalid recovery session identity.");
    require(result.stage2 && result.stage3 && result.stage2->mode == result.resultMode
                && result.stage3->mode == result.resultMode, "Incomplete recovered source captures.");
    const auto mode = number(identity.value("mode"));
    require(mode <= std::numeric_limits<std::uint32_t>::max(), "Invalid recovery file mode.");
    result.launchResult = {text(launch.value("text")),
        {number(identity.value("device")), number(identity.value("inode")), static_cast<std::uint32_t>(mode),
         signedNumber(identity.value("size")), signedNumber(identity.value("modifiedSeconds")),
         signedNumber(identity.value("modifiedNanoseconds")), signedNumber(identity.value("changedSeconds")),
         signedNumber(identity.value("changedNanoseconds"))}};
    return result;
}

OperationOutcome RecoveryRecordDecoder::outcome(const QJsonValue &value) const
{
    const auto object = value.toObject();
    return {number(object.value("revision")), enumeration<Outcome>(object.value("outcome"), 2), bytes(object.value("detail"))};
}
}
