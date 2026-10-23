#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace branch_truce::storage {
enum class TextEncoding { Utf8, Utf16LittleEndian, Utf16BigEndian };
enum class Newline { Lf, CrLf };

struct TextFormat {
    TextEncoding encoding = TextEncoding::Utf8;
    bool bom = false;
    Newline newline = Newline::Lf;
    bool finalNewline = false;
    bool operator==(const TextFormat &) const = default;
};
struct TextDocument {
    QByteArray utf8; // Editable UTF-8, LF line separators, no encoding BOM.
    QByteArray originalBytes;
    TextFormat format;
};
struct TextResult {
    std::optional<TextDocument> document;
    QString diagnostic;
};

// Unattributed text must be UTF-8. UTF-16 requires an explicit Git encoding.
[[nodiscard]] TextResult decodeText(const QByteArray &bytes, const QByteArray &workingTreeEncoding = {});
[[nodiscard]] TextResult serializeText(const QByteArray &utf8, const TextFormat &format);
[[nodiscard]] std::optional<TextFormat> resultFormat(const TextFormat &launch,
    const QByteArray &workingTreeEncoding, const QByteArray &eol, QString &diagnostic);
} // namespace branch_truce::storage
