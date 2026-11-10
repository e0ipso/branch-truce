// Recovery records encode a file's state at an acknowledgment boundary and read each field back typed.
#include "recovery/RecoveryRecord.h"
#include "core/ResourceLimits.h"
#include "storage/TextFormat.h"
#include <QJsonArray>
#include <QUuid>
#include <limits>

namespace branch_truce {
namespace {
constexpr int schema = 3;

void retain(qint64 &remaining, qint64 bytes)
{
    if (bytes < 0 || bytes > remaining) throw RecoveryRecordError(Problem{ProblemCode::RecoveryResourceLimit});
    remaining -= bytes;
}

void account(qint64 &used, qint64 bytes)
{
    constexpr qint64 limit = 256 * 1024 * 1024;
    auto remaining = limit - used;
    if (bytes < 0 || bytes > remaining) throw std::length_error("Live recovery checkpoint exceeds 256 MiB.");
    retain(remaining, bytes);
    used = limit - remaining;
}

// Field names, shared by the encoder and the reader.
namespace field {
constexpr QLatin1StringView schema("schema"), action("action"), fileId("fileId"), path("path"), support("support"),
    supportDetail("supportDetail"), snapshots("snapshots"), current("current"), checkpoint("checkpoint"),
    written("written"), reviewed("reviewed"), staged("staged"), checkpointOutcome("checkpointOutcome"),
    writeOutcome("writeOutcome"), stageOutcome("stageOutcome"), mutation("mutation"), alignment("alignment");
constexpr QLatin1StringView id("id"), result("result"), text("text"), decisions("decisions"), value("value"),
    order("order"), handEdited("handEdited"), handEditedRanges("handEditedRanges"), begin("begin"), end("end"),
    prefix("prefix"), suffix("suffix"), insert("insert"), revision("revision"),
    confirmedUndecided("confirmedUndecided"), objectId("objectId"), mode("mode"), outcome("outcome"), detail("detail"),
    kind("kind"), observation("observation");
constexpr QLatin1StringView utf8("utf8"), bytes("bytes"), format("format"), encoding("encoding"), bom("bom"),
    newline("newline"), finalNewline("finalNewline"), rawBytes("rawBytes"), worktree("worktree"), commitId("commitId"),
    label("label"), key("key"), resultMode("resultMode"), earlier("earlier"), stage2("stage2"), stage3("stage3"),
    conversion("conversion"), attributes("attributes"), configuration("configuration"), provenance("provenance"),
    sessionId("sessionId"), worktreeRoot("worktreeRoot"), gitDirectory("gitDirectory"), operation("operation"),
    stage2CommitId("stage2CommitId"), stage3CommitId("stage3CommitId"), baseCommitId("baseCommitId"),
    headName("headName"), originalHead("originalHead"), onto("onto"), launchResult("launchResult"),
    identity("identity"), device("device"), inode("inode"), size("size"), modifiedSeconds("modifiedSeconds"),
    modifiedNanoseconds("modifiedNanoseconds"), changedSeconds("changedSeconds"),
    changedNanoseconds("changedNanoseconds");
}

constexpr NameTable<DecisionValue, 3> decisionNames{{
    {DecisionValue::Undecided, "undecided"},
    {DecisionValue::Accepted, "accepted"},
    {DecisionValue::Ignored, "ignored"},
}};
constexpr NameTable<Support, 4> supportNames{{
    {Support::Supported, "supported"},
    {Support::OutsideApp, "outside-app"},
    {Support::Unreadable, "unreadable"},
    {Support::LargeFile, "large-file"},
}};
constexpr NameTable<Outcome, 3> outcomeNames{{
    {Outcome::Succeeded, "succeeded"},
    {Outcome::Failed, "failed"},
    {Outcome::Uncertain, "uncertain"},
}};
constexpr NameTable<SnapshotFormat::Encoding, 3> encodingNames{{
    {SnapshotFormat::Encoding::Utf8, "utf-8"},
    {SnapshotFormat::Encoding::Utf16LittleEndian, "utf-16le"},
    {SnapshotFormat::Encoding::Utf16BigEndian, "utf-16be"},
}};
constexpr NameTable<SnapshotFormat::Newline, 2> newlineNames{{
    {SnapshotFormat::Newline::Lf, "lf"},
    {SnapshotFormat::Newline::CrLf, "crlf"},
}};
constexpr NameTable<SnapshotProvenance::Operation, 2> operationNames{{
    {SnapshotProvenance::Operation::Merge, "merge"},
    {SnapshotProvenance::Operation::Rebase, "rebase"},
}};

template <typename Enum, std::size_t Size> Enum named(const NameTable<Enum, Size> &table, const QJsonValue &value)
{
    const auto found = valueNamed(table, value.toString());
    requireRecord(value.isString() && found.has_value(), "Unsupported recovery value.");
    return *found;
}

// The bytes that turn previous into current: a kept prefix and suffix of
// previous around the inserted bytes. Both lengths are the longest possible,
// so the same two texts always give the same change.
struct TextChange {
    std::size_t prefix = 0;
    std::size_t suffix = 0;
    std::string_view insert;
};
TextChange textChange(std::string_view previous, std::string_view current)
{
    TextChange change;
    const auto shorter = std::min(previous.size(), current.size());
    while (change.prefix < shorter && previous[change.prefix] == current[change.prefix]) ++change.prefix;
    while (change.suffix < shorter - change.prefix
           && previous[previous.size() - 1 - change.suffix] == current[current.size() - 1 - change.suffix])
        ++change.suffix;
    change.insert = current.substr(change.prefix, current.size() - change.prefix - change.suffix);
    return change;
}

class Encoder {
public:
    storage::RecoveryTransaction transaction;

    QString bytes(std::string_view value)
    {
        const QByteArray data(value.data(), static_cast<qsizetype>(value.size()));
        account(transaction.retainedBytes, data.size());
        const auto hash = storage::RecoveryStore::digest(data);
        transaction.payloads.insert(hash, data);
        return QString::fromLatin1(hash);
    }
    QJsonObject current(const Revision &value, std::string_view previousText)
    {
        account(transaction.retainedBytes, static_cast<qint64>(value.draft.text.size()));
        QJsonArray decisions;
        for (const auto &[id, decision] : value.draft.decisions) {
            QJsonObject entry{{field::id, bytes(id)}, {field::value, nameOf(decisionNames, decision.value)}};
            if (decision.order) entry.insert(field::order, QString::number(*decision.order));
            decisions.push_back(entry);
        }
        const auto change = textChange(previousText, value.draft.text);
        QJsonArray handEdited;
        QJsonArray ranges;
        if (value.correspondence) {
            for (const auto &id : value.correspondence->handEditedAnchors()) handEdited.push_back(bytes(id));
            for (const auto &range : value.correspondence->handEditedRanges())
                ranges.push_back(QJsonObject{{field::begin, QString::number(range.begin)},
                                             {field::end, QString::number(range.end)}});
        }
        return {{field::id, QString::number(value.id)},
                {field::result, QString::number(value.result)},
                {field::text,
                 QJsonObject{{field::prefix, QString::number(change.prefix)},
                             {field::suffix, QString::number(change.suffix)},
                             {field::insert, bytes(change.insert)}}},
                {field::decisions, decisions},
                {field::handEdited, handEdited},
                {field::handEditedRanges, ranges}};
    }
    static QJsonValue revision(const Revision *value)
    {
        if (!value) return QJsonValue::Null;
        return QJsonObject{{field::id, QString::number(value->id)}, {field::result, QString::number(value->result)}};
    }
    QJsonValue outcome(const std::optional<OperationOutcome> &value)
    {
        if (!value) return QJsonValue::Null;
        return QJsonObject{{field::revision, QString::number(value->revision)},
                           {field::outcome, nameOf(outcomeNames, value->outcome)},
                           {field::detail, bytes(value->detail)}};
    }
    QJsonObject text(const TextSnapshot &value)
    {
        return {{field::utf8, bytes(value.utf8)},
                {field::bytes, bytes(value.bytes)},
                {field::format,
                 QJsonObject{{field::encoding, nameOf(encodingNames, value.format.encoding)},
                             {field::bom, value.format.bom},
                             {field::newline, nameOf(newlineNames, value.format.newline)},
                             {field::finalNewline, value.format.finalNewline}}}};
    }
    QJsonValue source(const std::optional<SourceSnapshot> &value)
    {
        if (!value) return QJsonValue::Null;
        return QJsonObject{{field::objectId, bytes(value->objectId)},
                           {field::mode, bytes(value->mode)},
                           {field::rawBytes, bytes(value->rawBytes)},
                           {field::worktree, text(value->worktree)},
                           {field::commitId, value->commitId ? QJsonValue(bytes(*value->commitId)) : QJsonValue::Null},
                           {field::label, bytes(value->label)}};
    }
    QJsonArray mapping(const std::map<std::string, std::string> &values)
    {
        QJsonArray result;
        for (const auto &[key, value] : values)
            result.push_back(QJsonObject{{field::key, bytes(key)}, {field::value, bytes(value)}});
        return result;
    }
    QJsonValue snapshots(const FileSnapshots *value)
    {
        if (!value) return QJsonValue::Null;
        const auto &p = value->provenance;
        const auto &i = value->launchResult.identity;
        return QJsonObject{
            {field::path, bytes(value->path)},
            {field::resultMode, bytes(value->resultMode)},
            {field::earlier, source(value->earlier)},
            {field::stage2, source(value->stage2)},
            {field::stage3, source(value->stage3)},
            {field::conversion,
             QJsonObject{{field::attributes, mapping(value->conversion.attributes)},
                         {field::configuration, mapping(value->conversion.configuration)}}},
            {field::provenance,
             QJsonObject{{field::sessionId, bytes(p.sessionId)},
                         {field::worktreeRoot, bytes(p.worktreeRoot)},
                         {field::gitDirectory, bytes(p.gitDirectory)},
                         {field::operation, nameOf(operationNames, p.operation)},
                         {field::stage2CommitId, bytes(p.stage2CommitId)},
                         {field::stage3CommitId, bytes(p.stage3CommitId)},
                         {field::baseCommitId, p.baseCommitId ? QJsonValue(bytes(*p.baseCommitId)) : QJsonValue::Null},
                         {field::headName, bytes(p.headName)},
                         {field::originalHead, bytes(p.originalHead)},
                         {field::onto, bytes(p.onto)}}},
            {field::launchResult,
             QJsonObject{{field::text, text(value->launchResult.text)},
                         {field::identity,
                          QJsonObject{{field::device, QString::number(i.device)},
                                      {field::inode, QString::number(i.inode)},
                                      {field::mode, QString::number(i.mode)},
                                      {field::size, QString::number(i.size)},
                                      {field::modifiedSeconds, QString::number(i.modifiedSeconds)},
                                      {field::modifiedNanoseconds, QString::number(i.modifiedNanoseconds)},
                                      {field::changedSeconds, QString::number(i.changedSeconds)},
                                      {field::changedNanoseconds, QString::number(i.changedNanoseconds)}}}}}};
    }
};

// The stored revision a publication outcome names. The session keeps every one.
const Revision *revisionOf(const FileSession &file, std::optional<RevisionId> id)
{
    return id ? file.revision(*id) : nullptr;
}
}

storage::RecoveryTransaction recoveryTransaction(const FileSession &file, RecoveryAction action,
                                                 const QString &transactionId, std::string_view previousText,
                                                 const std::optional<MutationFact> &mutation)
{
    Encoder encoder;
    const auto &publication = file.publication();
    encoder.transaction.id = transactionId;
    encoder.transaction.continuation = [algorithm = file.algorithm()](const storage::RecoveryStore &store) {
        return recoveryCheckpoint(store, algorithm);
    };
    auto &state = encoder.transaction.state;
    state = {{field::schema, schema},
             {field::action, nameOf(recoveryActionNames, action)},
             {field::fileId, encoder.bytes(file.id())},
             {field::path, encoder.bytes(file.path())},
             {field::support, nameOf(supportNames, file.support())},
             {field::supportDetail, encoder.bytes(file.supportDetail())},
             {field::current, encoder.current(file.current(), previousText)},
             {field::checkpoint, Encoder::revision(revisionOf(file, publication.checkpoint()))},
             {field::written, Encoder::revision(revisionOf(file, publication.written()))},
             {field::checkpointOutcome, encoder.outcome(publication.checkpointOutcome())},
             {field::writeOutcome, encoder.outcome(publication.writeOutcome())},
             {field::stageOutcome, encoder.outcome(publication.stageOutcome())}};
    if (action == RecoveryAction::Open) {
        state.insert(field::snapshots, encoder.snapshots(file.snapshots()));
        // Decision IDs come from this algorithm's passages.
        state.insert(field::alignment, encoder.bytes(file.algorithm()->identity()));
    }
    if (const auto &reviewed = publication.reviewed())
        state.insert(field::reviewed,
                     QJsonObject{{field::revision, Encoder::revision(revisionOf(file, reviewed->revision))},
                                 {field::confirmedUndecided, reviewed->confirmedUndecided}});
    else
        state.insert(field::reviewed, QJsonValue::Null);
    if (const auto &staged = publication.staged())
        state.insert(field::staged,
                     QJsonObject{{field::revision, Encoder::revision(revisionOf(file, staged->revision))},
                                 {field::objectId, encoder.bytes(staged->objectId)},
                                 {field::mode, encoder.bytes(staged->mode)},
                                 {field::confirmedUndecided, staged->confirmedUndecided}});
    else
        state.insert(field::staged, QJsonValue::Null);
    if (mutation) {
        QJsonObject fact{{field::id, mutation->id}, {field::kind, nameOf(mutationKindNames, mutation->kind)}};
        if (mutation->observation)
            fact.insert(field::observation, nameOf(mutationObservationNames, *mutation->observation));
        state.insert(field::mutation, fact);
    }
    return std::move(encoder.transaction);
}

storage::RecoveryTransaction recoveryFileCheckpoint(const FileSession &file)
{
    Encoder encoder;
    encoder.transaction = recoveryTransaction(file, RecoveryAction::Open, {}, {});
    const auto checkpoint = SessionCheckpoint::capture(file);
    const auto &history = checkpoint.history;
    QJsonArray revisions, entries;
    for (const auto &revision : history.revisions) {
        auto value = encoder.current(*revision, {});
        value.insert("decisionsId", QString::number(revision->decisions));
        revisions.push_back(value);
    }
    for (auto id : history.entries) entries.push_back(QString::number(id));
    encoder.transaction.state.insert("history",
                                     QJsonObject{{"revisions", revisions},
                                                 {"entries", entries},
                                                 {"cursor", QString::number(history.cursor)},
                                                 {"current", QString::number(history.current)},
                                                 {"launch", QString::number(history.launch)},
                                                 {"next", QString::number(history.next)},
                                                 {"aligned", checkpoint.aligned},
                                                 {"floor", QString::number(checkpoint.publication.floor)},
                                                 {"reviewSaved", checkpoint.publication.reviewSaved}});
    return std::move(encoder.transaction);
}

FileSession RecoveryRecordReader::checkpointFile(std::shared_ptr<const CorrespondenceAlgorithm> correspondence) const
{
    const auto history = state_.value("history").toObject();
    SessionCheckpoint imported;
    auto &checkpoint = imported.history;
    auto &publication = imported.publication;
    imported.aligned = boolean(history.value("aligned"));
    publication.floor = number(history.value("floor"));
    publication.reviewSaved = boolean(history.value("reviewSaved"));
    checkpoint.current = number(history.value("current"));
    checkpoint.launch = number(history.value("launch"));
    checkpoint.next = number(history.value("next"));
    checkpoint.cursor = number(history.value("cursor"));
    requireRecord(history.value("entries").isArray() && history.value("revisions").isArray(),
                  "Invalid recovery checkpoint history.");
    for (const auto &entry : history.value("entries").toArray()) checkpoint.entries.push_back(number(entry));
    requireRecord(!checkpoint.entries.empty() && checkpoint.entries.size() <= kMaxHistoryEntries
                      && checkpoint.cursor < checkpoint.entries.size()
                      && history.value("revisions").toArray().size() <= static_cast<qsizetype>(kMaxHistoryEntries + 6),
                  "Checkpoint history exceeds supported reachability.");
    std::vector<RecordedRevision> revisions;
    for (const auto &entry : history.value("revisions").toArray()) {
        auto state = state_;
        state.insert(field::current, entry);
        RecoveryRecordReader reader(store_, state, remainingBytes_, encodedPayloads_);
        auto revision = reader.revision({});
        revision.decisions = number(entry.toObject().value("decisionsId"));
        remainingBytes_ = reader.remainingBytes_;
        revisions.push_back(std::move(revision));
    }
    const auto id = fileId(), filePath = path();
    const auto launch = std::find_if(revisions.begin(), revisions.end(),
                                     [&](const auto &value) { return value.id == checkpoint.launch; });
    requireRecord(!id.empty() && !filePath.empty() && filePath.find('\0') == std::string::npos
                      && launch != revisions.end(),
                  "Invalid checkpoint file identity or launch.");
    FileSession file(id, filePath, launch->draft,
                     {.support = support(),
                      .supportDetail = supportDetail(),
                      .snapshots = snapshots(),
                      .algorithm = correspondence});
    if (support() == Support::Supported && file.support() == Support::LargeFile)
        requireRecord(file.loadLargeFile() == Transition::Applied, "Unsupported checkpoint large file.");
    requireRecord(file.algorithm()->identity() == alignment(), "Unsupported checkpoint correspondence.");
    const auto reference = [&](QJsonValue value) -> std::optional<RevisionId> {
        return value.isNull() ? std::nullopt : std::optional<RevisionId>{number(value.toObject().value(field::id))};
    };
    publication.checkpoint = reference(state_.value(field::checkpoint));
    publication.written = reference(state_.value(field::written));
    const auto reviewed = state_.value(field::reviewed);
    if (!reviewed.isNull())
        publication.reviewed = Review{reference(reviewed.toObject().value(field::revision)).value(),
                                      boolean(reviewed.toObject().value(field::confirmedUndecided))};
    const auto staged = state_.value(field::staged);
    if (!staged.isNull()) {
        const auto [object, mode] = stagedObject();
        publication.staged = StagedResult{reference(staged.toObject().value(field::revision)).value(), object, mode,
                                          boolean(staged.toObject().value(field::confirmedUndecided))};
    }
    const auto readOutcome = [&](QLatin1StringView key) -> std::optional<OperationOutcome> {
        return state_.value(key).isNull() ? std::nullopt : std::optional{outcome(state_.value(key))};
    };
    publication.checkpointOutcome = readOutcome(field::checkpointOutcome);
    publication.writeOutcome = readOutcome(field::writeOutcome);
    publication.stageOutcome = readOutcome(field::stageOutcome);
    std::move(imported).restore(file, std::move(revisions));
    const auto encoded = recoveryFileCheckpoint(file);
    requireRecord(encoded.state == state_, "Noncanonical recovery checkpoint.");
    return file;
}

void requireRecord(bool condition, const char *detail)
{
    if (!condition) throw RecoveryRecordError(detail);
}

RecoveryRecordReader::RecoveryRecordReader(const storage::RecoveryStore &store, const QJsonObject &state,
                                           qint64 retainedBytes, const QMap<QByteArray, QByteArray> *encodedPayloads)
    : store_(store), state_(state), remainingBytes_(retainedBytes), encodedPayloads_(encodedPayloads)
{
    requireRecord(state_.value(field::schema).toInt() == schema, "Unsupported recovery state schema.");
}

RecoveryAction RecoveryRecordReader::action() const { return named(recoveryActionNames, state_.value(field::action)); }

std::string RecoveryRecordReader::fileId() const { return bytes(state_.value(field::fileId)); }
std::string RecoveryRecordReader::path() const { return bytes(state_.value(field::path)); }
Support RecoveryRecordReader::support() const { return named(supportNames, state_.value(field::support)); }
std::string RecoveryRecordReader::supportDetail() const { return bytes(state_.value(field::supportDetail)); }

QByteArray RecoveryRecordReader::payload(const QByteArray &digest, Problem *problem, qint64 maxBytes) const
{
    if (!encodedPayloads_) return store_.payload(digest, problem, maxBytes);
    const auto found = encodedPayloads_->constFind(digest);
    if (found == encodedPayloads_->cend() || storage::RecoveryStore::digest(*found) != digest) {
        *problem = ProblemCode::RecoveryPayloadIntegrity;
        return {};
    }
    if (found->size() > maxBytes) {
        *problem = ProblemCode::RecoveryResourceLimit;
        return {};
    }
    *problem = {};
    return *found;
}

std::string RecoveryRecordReader::bytes(const QJsonValue &value) const
{
    requireRecord(value.isString(), "Missing recovery payload reference.");
    Problem error;
    const auto result = payload(value.toString().toLatin1(), &error, remainingBytes_);
    if (error.code == ProblemCode::RecoveryResourceLimit) throw RecoveryRecordError(error);
    requireRecord(error.isEmpty(), "Recovery payload is missing, unsafe or corrupt.");
    retain(remainingBytes_, result.size());
    return result.toStdString();
}

quint64 RecoveryRecordReader::number(const QJsonValue &value) const
{
    bool ok = false;
    const auto result = value.toString().toULongLong(&ok);
    requireRecord(ok && QString::number(result) == value.toString(), "Invalid recovery integer.");
    return result;
}

qint64 RecoveryRecordReader::signedNumber(const QJsonValue &value) const
{
    bool ok = false;
    const auto result = value.toString().toLongLong(&ok);
    requireRecord(ok && QString::number(result) == value.toString(), "Invalid recovery signed integer.");
    return result;
}

bool RecoveryRecordReader::boolean(const QJsonValue &value) const
{
    requireRecord(value.isBool(), "Invalid recovery boolean.");
    return value.toBool();
}

Draft RecoveryRecordReader::draft(std::string_view previousText) const
{
    const auto current = state_.value(field::current).toObject();
    const auto change = current.value(field::text).toObject();
    const auto prefix = number(change.value(field::prefix));
    const auto suffix = number(change.value(field::suffix));
    requireRecord(prefix <= previousText.size() && suffix <= previousText.size() - prefix,
                  "Invalid recovered text change.");
    retain(remainingBytes_, static_cast<qint64>(prefix + suffix));
    Draft result;
    result.text = std::string(previousText.substr(0, prefix)) + bytes(change.value(field::insert))
        + std::string(previousText.substr(previousText.size() - suffix));
    requireRecord(validUtf8(result.text) && current.value(field::decisions).isArray(), "Invalid recovered draft.");
    for (const auto &entry : current.value(field::decisions).toArray()) {
        const auto decision = entry.toObject();
        Decision value{named(decisionNames, decision.value(field::value)), {}};
        if (decision.contains(field::order)) value.order = number(decision.value(field::order));
        requireRecord(result.decisions.emplace(bytes(decision.value(field::id)), value).second,
                      "Duplicate recovered decision.");
    }
    return result;
}

RecordedRevision RecoveryRecordReader::revision(std::string_view previousText) const
{
    const auto current = state_.value(field::current).toObject();
    return {number(current.value(field::id)), number(current.value(field::result)), draft(previousText), handEdited(),
            handEditedRanges()};
}

std::set<std::string> RecoveryRecordReader::handEdited() const
{
    const auto value = state_.value(field::current).toObject().value(field::handEdited);
    requireRecord(value.isArray(), "Invalid recovered hand edits.");
    std::set<std::string> result;
    for (const auto &entry : value.toArray())
        requireRecord(result.insert(bytes(entry)).second, "Duplicate recovered hand edit.");
    return result;
}

std::vector<ByteRange> RecoveryRecordReader::handEditedRanges() const
{
    const auto value = state_.value(field::current).toObject().value(field::handEditedRanges);
    requireRecord(value.isArray(), "Invalid recovered hand-edited ranges.");
    std::vector<ByteRange> result;
    for (const auto &entry : value.toArray()) {
        requireRecord(entry.isObject(), "Invalid recovered hand-edited range.");
        const auto range = entry.toObject();
        const ByteRange decoded{static_cast<std::size_t>(number(range.value(field::begin))),
                                static_cast<std::size_t>(number(range.value(field::end)))};
        requireRecord(decoded.begin <= decoded.end, "Invalid recovered hand-edited range.");
        result.push_back(decoded);
    }
    return result;
}

bool RecoveryRecordReader::reviewConfirmedUndecided() const
{
    return boolean(state_.value(field::reviewed).toObject().value(field::confirmedUndecided));
}

OperationOutcome RecoveryRecordReader::outcome(const QJsonValue &value) const
{
    requireRecord(value.isObject(), "Missing recovery outcome.");
    const auto object = value.toObject();
    return {number(object.value(field::revision)), named(outcomeNames, object.value(field::outcome)),
            bytes(object.value(field::detail))};
}
OperationOutcome RecoveryRecordReader::checkpointOutcome() const
{
    return outcome(state_.value(field::checkpointOutcome));
}
OperationOutcome RecoveryRecordReader::writeOutcome() const { return outcome(state_.value(field::writeOutcome)); }
OperationOutcome RecoveryRecordReader::stageOutcome() const { return outcome(state_.value(field::stageOutcome)); }

std::pair<std::string, std::string> RecoveryRecordReader::stagedObject() const
{
    const auto staged = state_.value(field::staged).toObject();
    return {bytes(staged.value(field::objectId)), bytes(staged.value(field::mode))};
}

std::optional<MutationFact> RecoveryRecordReader::mutation() const
{
    if (!state_.contains(field::mutation)) return std::nullopt;
    const auto object = state_.value(field::mutation).toObject();
    MutationFact fact{object.value(field::id).toString(), named(mutationKindNames, object.value(field::kind)), {}};
    requireRecord(!QUuid(fact.id).isNull(), "Invalid recovery mutation identity.");
    if (object.contains(field::observation))
        fact.observation = named(mutationObservationNames, object.value(field::observation));
    return fact;
}

TextSnapshot RecoveryRecordReader::text(const QJsonValue &value) const
{
    const auto object = value.toObject();
    const auto format = object.value(field::format).toObject();
    TextSnapshot result{bytes(object.value(field::utf8)),
                        bytes(object.value(field::bytes)),
                        {named(encodingNames, format.value(field::encoding)), boolean(format.value(field::bom)),
                         named(newlineNames, format.value(field::newline)),
                         boolean(format.value(field::finalNewline))}};
    requireRecord(validUtf8(result.utf8), "Invalid recovered text encoding.");
    const auto decoded =
        storage::decodeText(QByteArray::fromStdString(result.bytes), storage::encodingName(result.format.encoding));
    requireRecord(decoded.document && decoded.document->utf8.toStdString() == result.utf8,
                  "Recovered text bytes and editable representation disagree.");
    requireRecord(decoded.document->format == result.format, "Recovered text format does not describe its bytes.");
    return result;
}

std::optional<SourceSnapshot> RecoveryRecordReader::source(const QJsonValue &value) const
{
    if (value.isNull()) return {};
    const auto object = value.toObject();
    SourceSnapshot result{bytes(object.value(field::objectId)),
                          bytes(object.value(field::mode)),
                          bytes(object.value(field::rawBytes)),
                          text(object.value(field::worktree)),
                          {},
                          bytes(object.value(field::label))};
    if (!object.value(field::commitId).isNull()) result.commitId = bytes(object.value(field::commitId));
    requireRecord(!result.objectId.empty() && (result.mode == "100644" || result.mode == "100755"),
                  "Invalid recovered source identity.");
    return result;
}

std::map<std::string, std::string> RecoveryRecordReader::mapping(const QJsonValue &value) const
{
    requireRecord(value.isArray(), "Invalid recovery conversion policy.");
    std::map<std::string, std::string> result;
    for (const auto &entry : value.toArray()) {
        const auto object = entry.toObject();
        requireRecord(result.emplace(bytes(object.value(field::key)), bytes(object.value(field::value))).second,
                      "Duplicate recovery conversion policy entry.");
    }
    return result;
}

std::string RecoveryRecordReader::alignment() const { return bytes(state_.value(field::alignment)); }

std::string RecoveryRecordReader::worktreeRoot() const
{
    if (action() != RecoveryAction::Open) throw RecoveryRecordError(Problem{ProblemCode::RecoveryRecordSchema});
    const auto root =
        state_.value(field::snapshots).toObject().value(field::provenance).toObject().value(field::worktreeRoot);
    if (!root.isString()) throw RecoveryRecordError(Problem{ProblemCode::RecoveryRecordSchema});
    Problem error;
    const auto value = payload(root.toString().toLatin1(), &error, 4096);
    if (!error.isEmpty() || !value.startsWith('/') || value.contains('\0'))
        throw RecoveryRecordError(Problem{ProblemCode::RecoveryRecordSchema});
    retain(remainingBytes_, value.size());
    return value.toStdString();
}

std::optional<FileSnapshots> RecoveryRecordReader::snapshots() const
{
    const auto value = state_.value(field::snapshots);
    requireRecord(value.isNull() || value.isObject(), "Invalid recovery capture.");
    if (value.isNull()) return {};
    const auto object = value.toObject();
    const auto p = object.value(field::provenance).toObject();
    const auto launch = object.value(field::launchResult).toObject();
    const auto identity = launch.value(field::identity).toObject();
    FileSnapshots result;
    result.path = bytes(object.value(field::path));
    result.resultMode = bytes(object.value(field::resultMode));
    result.earlier = source(object.value(field::earlier));
    result.stage2 = source(object.value(field::stage2));
    result.stage3 = source(object.value(field::stage3));
    const auto policy = object.value(field::conversion).toObject();
    result.conversion = {mapping(policy.value(field::attributes)), mapping(policy.value(field::configuration))};
    auto &provenance = result.provenance;
    provenance.sessionId = bytes(p.value(field::sessionId));
    provenance.worktreeRoot = worktreeRoot();
    provenance.gitDirectory = bytes(p.value(field::gitDirectory));
    provenance.operation = named(operationNames, p.value(field::operation));
    provenance.stage2CommitId = bytes(p.value(field::stage2CommitId));
    provenance.stage3CommitId = bytes(p.value(field::stage3CommitId));
    if (!p.value(field::baseCommitId).isNull()) provenance.baseCommitId = bytes(p.value(field::baseCommitId));
    provenance.headName = bytes(p.value(field::headName));
    provenance.originalHead = bytes(p.value(field::originalHead));
    provenance.onto = bytes(p.value(field::onto));
    requireRecord(!QUuid(QString::fromStdString(provenance.sessionId)).isNull(), "Invalid recovery session identity.");
    requireRecord(result.stage2 && result.stage3 && result.stage2->mode == result.resultMode
                      && result.stage3->mode == result.resultMode,
                  "Incomplete recovered source captures.");
    const auto mode = number(identity.value(field::mode));
    requireRecord(mode <= std::numeric_limits<std::uint32_t>::max(), "Invalid recovery file mode.");
    result.launchResult = {
        text(launch.value(field::text)),
        {number(identity.value(field::device)), number(identity.value(field::inode)), static_cast<std::uint32_t>(mode),
         signedNumber(identity.value(field::size)), signedNumber(identity.value(field::modifiedSeconds)),
         signedNumber(identity.value(field::modifiedNanoseconds)), signedNumber(identity.value(field::changedSeconds)),
         signedNumber(identity.value(field::changedNanoseconds))}};
    return result;
}
}
