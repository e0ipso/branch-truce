// Mergetool messages are the typed frames a launcher and a session exchange, with their framing and version check.
#pragma once

#include "core/RevisionId.h"
#include "git/GitTypes.h"
#include "problems/Problem.h"
#include <QByteArray>
#include <QString>
#include <optional>
#include <variant>

namespace branch_truce {

// The launcher and the window ship in one package. A peer that speaks another
// version gets an Unsupported reply naming this one, and neither side guesses.
inline constexpr int kMergetoolProtocolVersion = 3;
// Largest payload of one frame. A frame is a 4-byte big-endian length, then
// that many bytes of one compact JSON object.
inline constexpr qsizetype kMergetoolFrameLimit = 65536;

// Launcher to session: Git launched this invocation and its inputs are copied
// to intake. configuration is what the Git that launched it reads.
struct AttachMessage {
    QString invocation;
    QString intake;
    git::GitEnvironment configuration;
    bool operator==(const AttachMessage &) const = default;
};
// Session to launcher: the launch waits for a decision.
struct AttachedMessage {
    QString invocation;
    bool operator==(const AttachedMessage &) const = default;
};
// Session to launcher: the launch cannot wait here, and why, translated by the
// session; the launcher prints it as it is.
struct RejectedMessage {
    QString invocation;
    QString reason;
    bool operator==(const RejectedMessage &) const = default;
};
// Either side: the peer's frame used another protocol version or an unknown
// kind. Carries the version this side speaks; it is readable at any version.
struct UnsupportedMessage {
    int version = kMergetoolProtocolVersion;
    bool operator==(const UnsupportedMessage &) const = default;
};
// Session to launcher: this revision is written; exit 0 so Git stages it.
struct ResolvedMessage {
    QString invocation;
    RevisionId revision = 0;
    QByteArray object; // Canonical object id of the written result.
    bool operator==(const ResolvedMessage &) const = default;
};
// Session to launcher: leave the path unresolved and restore MERGED.
struct LeaveMessage {
    QString invocation;
    bool operator==(const LeaveMessage &) const = default;
};
using MergetoolMessage =
    std::variant<AttachMessage, AttachedMessage, RejectedMessage, UnsupportedMessage, ResolvedMessage, LeaveMessage>;

enum class MergetoolDecodeFailure { None, Malformed, OtherVersion, UnknownKind };
struct DecodedMergetoolMessage {
    std::optional<MergetoolMessage> message;
    MergetoolDecodeFailure failure = MergetoolDecodeFailure::None;
    // The version the frame named, when it named one.
    int version = 0;
    Problem problem;
};

// One JSON payload, without the length prefix.
[[nodiscard]] QByteArray encodeMergetoolMessage(const MergetoolMessage &message);
// Checks the version and every field of the frame's kind. Unknown fields are ignored.
[[nodiscard]] DecodedMergetoolMessage decodeMergetoolMessage(const QByteArray &payload);

// The payload with its length prefix. Empty when the payload is empty or over the limit.
[[nodiscard]] QByteArray mergetoolFrame(const QByteArray &payload);
enum class MergetoolFrame { NeedMore, Ready, Invalid };
// Takes one frame's payload off the front of buffer. Invalid for a zero or
// oversized length; nothing is taken then.
[[nodiscard]] MergetoolFrame takeMergetoolFrame(QByteArray *buffer, QByteArray *payload);
}
