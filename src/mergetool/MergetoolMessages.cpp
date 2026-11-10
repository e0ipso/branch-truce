// Mergetool messages are the typed frames a launcher and a session exchange, with their framing and version check.
#include "mergetool/MergetoolMessages.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <array>
#include <utility>

namespace branch_truce {
namespace {
enum class Kind { Attach, Attached, Rejected, Unsupported, Resolved, Leave };
constexpr std::array<std::pair<Kind, const char *>, 6> kKinds{{{Kind::Attach, "attach"},
                                                               {Kind::Attached, "attached"},
                                                               {Kind::Rejected, "rejected"},
                                                               {Kind::Unsupported, "unsupported"},
                                                               {Kind::Resolved, "resolved"},
                                                               {Kind::Leave, "leave"}}};

QString kindName(Kind kind)
{
    for (const auto &[value, name] : kKinds)
        if (value == kind) return QString::fromLatin1(name);
    return {};
}

std::optional<Kind> parseKind(const QString &name)
{
    for (const auto &[value, text] : kKinds)
        if (name == QLatin1String(text)) return value;
    return std::nullopt;
}

bool objectId(const QByteArray &id)
{
    return (id.size() == 40 || id.size() == 64)
        && std::all_of(id.cbegin(), id.cend(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

QJsonObject configurationObject(const git::GitEnvironment &configuration)
{
    QJsonArray entries;
    for (const auto &[key, value] : configuration.entries) entries.append(QJsonArray{key, value});
    return {{QStringLiteral("home"), configuration.home},
            {QStringLiteral("configHome"), configuration.configHome},
            {QStringLiteral("globalConfig"), configuration.globalConfig},
            {QStringLiteral("systemConfig"), configuration.systemConfig},
            {QStringLiteral("noSystem"), configuration.noSystem},
            {QStringLiteral("parameters"), configuration.parameters},
            {QStringLiteral("entries"), entries}};
}

std::optional<git::GitEnvironment> configurationFrom(const QJsonValue &value)
{
    if (!value.isObject()) return std::nullopt;
    const auto object = value.toObject();
    git::GitEnvironment configuration;
    for (const auto &[key, field] : {std::pair{QStringLiteral("home"), &configuration.home},
                                     std::pair{QStringLiteral("configHome"), &configuration.configHome},
                                     std::pair{QStringLiteral("globalConfig"), &configuration.globalConfig},
                                     std::pair{QStringLiteral("systemConfig"), &configuration.systemConfig},
                                     std::pair{QStringLiteral("noSystem"), &configuration.noSystem},
                                     std::pair{QStringLiteral("parameters"), &configuration.parameters}}) {
        if (!object.value(key).isString()) return std::nullopt;
        *field = object.value(key).toString();
    }
    const auto entries = object.value(QStringLiteral("entries"));
    if (!entries.isArray()) return std::nullopt;
    for (const auto &entry : entries.toArray()) {
        const auto pair = entry.toArray();
        if (!entry.isArray() || pair.size() != 2 || !pair[0].isString() || !pair[1].isString()) return std::nullopt;
        configuration.entries.append({pair[0].toString(), pair[1].toString()});
    }
    return configuration;
}

struct Encoder {
    QJsonObject operator()(const AttachMessage &message) const
    {
        return {{QStringLiteral("kind"), kindName(Kind::Attach)},
                {QStringLiteral("invocation"), message.invocation},
                {QStringLiteral("intake"), message.intake},
                {QStringLiteral("configuration"), configurationObject(message.configuration)}};
    }
    QJsonObject operator()(const AttachedMessage &message) const
    {
        return {{QStringLiteral("kind"), kindName(Kind::Attached)}, {QStringLiteral("invocation"), message.invocation}};
    }
    QJsonObject operator()(const RejectedMessage &message) const
    {
        return {{QStringLiteral("kind"), kindName(Kind::Rejected)},
                {QStringLiteral("invocation"), message.invocation},
                {QStringLiteral("reason"), message.reason}};
    }
    QJsonObject operator()(const UnsupportedMessage &message) const
    {
        return {{QStringLiteral("kind"), kindName(Kind::Unsupported)}, {QStringLiteral("v"), message.version}};
    }
    QJsonObject operator()(const ResolvedMessage &message) const
    {
        return {{QStringLiteral("kind"), kindName(Kind::Resolved)},
                {QStringLiteral("invocation"), message.invocation},
                {QStringLiteral("revision"), QString::number(message.revision)},
                {QStringLiteral("object"), QString::fromLatin1(message.object)}};
    }
    QJsonObject operator()(const LeaveMessage &message) const
    {
        return {{QStringLiteral("kind"), kindName(Kind::Leave)}, {QStringLiteral("invocation"), message.invocation}};
    }
};

DecodedMergetoolMessage failed(MergetoolDecodeFailure failure, int version, Problem problem)
{
    DecodedMergetoolMessage decoded;
    decoded.failure = failure;
    decoded.version = version;
    decoded.problem = std::move(problem);
    return decoded;
}
} // namespace

QByteArray encodeMergetoolMessage(const MergetoolMessage &message)
{
    auto object = std::visit(Encoder{}, message);
    // Unsupported names its own version in "v"; every other frame names the sender's.
    if (!std::holds_alternative<UnsupportedMessage>(message))
        object.insert(QStringLiteral("v"), kMergetoolProtocolVersion);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

DecodedMergetoolMessage decodeMergetoolMessage(const QByteArray &payload)
{
    const Problem malformed(ProblemCode::MergetoolMessageInvalid);
    const auto document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) return failed(MergetoolDecodeFailure::Malformed, 0, malformed);
    const auto object = document.object();
    const auto versionValue = object.value(QStringLiteral("v"));
    const int version = versionValue.isDouble() ? versionValue.toInt() : 0;
    const auto kind = parseKind(object.value(QStringLiteral("kind")).toString());
    // Any version can read an Unsupported reply, so a peer always learns why.
    if (kind == Kind::Unsupported) {
        if (version <= 0) return failed(MergetoolDecodeFailure::Malformed, 0, malformed);
        return {UnsupportedMessage{version}, MergetoolDecodeFailure::None, version, {}};
    }
    if (version != kMergetoolProtocolVersion)
        return failed(MergetoolDecodeFailure::OtherVersion, version,
                      {ProblemCode::MergetoolOtherVersion,
                       {QString::number(version), QString::number(kMergetoolProtocolVersion)}});
    if (!kind) return failed(MergetoolDecodeFailure::UnknownKind, version, malformed);
    const auto text = [&object](const char *key) { return object.value(QLatin1String(key)).toString(); };
    const auto invocation = text("invocation");
    const bool validInvocation = !QUuid::fromString(invocation).isNull();
    std::optional<MergetoolMessage> message;
    switch (*kind) {
    case Kind::Attach: {
        const auto configuration = configurationFrom(object.value(QStringLiteral("configuration")));
        if (validInvocation && text("intake").startsWith(QLatin1Char('/')) && configuration)
            message = AttachMessage{invocation, text("intake"), *configuration};
        break;
    }
    case Kind::Attached:
        if (validInvocation) message = AttachedMessage{invocation};
        break;
    case Kind::Rejected:
        // A session that could not read the attach rejects it without an invocation.
        if ((validInvocation || invocation.isEmpty()) && !text("reason").isEmpty())
            message = RejectedMessage{invocation, text("reason")};
        break;
    case Kind::Resolved: {
        bool number = false;
        const auto revision = text("revision").toULongLong(&number);
        const auto objectName = text("object").toLatin1();
        if (validInvocation && number && revision > 0 && QString::number(revision) == text("revision")
            && objectId(objectName))
            message = ResolvedMessage{invocation, revision, objectName};
        break;
    }
    case Kind::Leave:
        if (validInvocation) message = LeaveMessage{invocation};
        break;
    case Kind::Unsupported: break;
    }
    if (!message) return failed(MergetoolDecodeFailure::Malformed, version, malformed);
    return {std::move(message), MergetoolDecodeFailure::None, version, {}};
}

QByteArray mergetoolFrame(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > kMergetoolFrameLimit) return {};
    const auto length = static_cast<quint32>(payload.size());
    QByteArray frame;
    frame.reserve(4 + payload.size());
    frame.append(static_cast<char>((length >> 24) & 0xffU));
    frame.append(static_cast<char>((length >> 16) & 0xffU));
    frame.append(static_cast<char>((length >> 8) & 0xffU));
    frame.append(static_cast<char>(length & 0xffU));
    frame.append(payload);
    return frame;
}

MergetoolFrame takeMergetoolFrame(QByteArray *buffer, QByteArray *payload)
{
    if (!buffer || !payload) return MergetoolFrame::Invalid;
    if (buffer->size() < 4) return MergetoolFrame::NeedMore;
    const auto byte = [buffer](qsizetype index) {
        return static_cast<quint32>(static_cast<unsigned char>(buffer->at(index)));
    };
    const auto length = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    if (length == 0 || length > static_cast<quint32>(kMergetoolFrameLimit)) return MergetoolFrame::Invalid;
    if (buffer->size() < 4 + static_cast<qsizetype>(length)) return MergetoolFrame::NeedMore;
    *payload = buffer->mid(4, static_cast<qsizetype>(length));
    buffer->remove(0, 4 + static_cast<qsizetype>(length));
    return MergetoolFrame::Ready;
}
}
