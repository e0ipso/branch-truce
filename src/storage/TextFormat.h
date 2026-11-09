// Text formatting decodes captured files and serializes results with their retained encoding and line endings.
#pragma once

#include "core/FileSnapshot.h"
#include <QByteArray>
#include <QString>
#include <optional>

namespace branch_truce::storage {
// The engine's format value; storage reads and writes it directly. These are
// names for it, not another type.
using TextFormat = SnapshotFormat;
using TextEncoding = SnapshotFormat::Encoding;
using Newline = SnapshotFormat::Newline;

struct TextDocument {
    QByteArray utf8; // Editable UTF-8, LF line separators, no encoding BOM.
    QByteArray originalBytes;
    TextFormat format;
};
// Why text could not be decoded, serialized or given a result format.
enum class TextFailure {
    None,
    InvalidUtf8,
    UnsupportedEncoding,
    MissingByteOrderMark,
    ContradictoryByteOrderMark,
    TruncatedUtf16,
    InvalidSurrogate,
    ControlCharacters,
    BareCarriageReturn,
    MixedNewlines,
    NoRoundTrip,
    InvalidEditableText,
    UnsupportedEol
};
struct TextResult {
    std::optional<TextDocument> document;
    TextFailure failure = TextFailure::None;
    QString diagnostic;
};

// The working-tree-encoding name decodeText takes for an encoding.
[[nodiscard]] QByteArray encodingName(SnapshotFormat::Encoding encoding);
// Unattributed text must be UTF-8. UTF-16 requires an explicit Git encoding.
[[nodiscard]] TextResult decodeText(const QByteArray &bytes, const QByteArray &workingTreeEncoding = {});
[[nodiscard]] TextResult serializeText(const QByteArray &utf8, const TextFormat &format);
[[nodiscard]] std::optional<TextFormat> resultFormat(const TextFormat &launch, const QByteArray &workingTreeEncoding,
                                                     const QByteArray &eol, QString &diagnostic);
} // namespace branch_truce::storage
