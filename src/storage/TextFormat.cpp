// Text formatting decodes captured files and serializes results with their retained encoding and line endings.
#include "TextFormat.h"

#include <string>

namespace branch_truce::storage {
namespace {
QByteArray encodingName(QByteArray name)
{
    name = name.toUpper();
    name.replace("-", "");
    return name;
}
bool absent(const QByteArray &value)
{
    return value.isEmpty() || value == "unspecified" || value == "unset";
}
bool textCharacters(const QString &text)
{
    for (const auto character : text) {
        const auto value = character.unicode();
        if ((value < 0x20 && value != '\t' && value != '\n' && value != '\r') || value == 0x7f) return false;
    }
    return true;
}
QByteArray encode(const QString &text, const TextFormat &format)
{
    if (format.encoding == TextEncoding::Utf8) {
        return (format.bom ? QByteArray::fromHex("efbbbf") : QByteArray{}) + text.toUtf8();
    }
    QByteArray bytes;
    if (format.bom) bytes = QByteArray::fromHex(format.encoding == TextEncoding::Utf16LittleEndian ? "fffe" : "feff");
    for (const auto character : text) {
        const auto value = character.unicode();
        const char low = static_cast<char>(value & 0xff);
        const char high = static_cast<char>(value >> 8);
        if (format.encoding == TextEncoding::Utf16LittleEndian) {
            bytes += low;
            bytes += high;
        } else {
            bytes += high;
            bytes += low;
        }
    }
    return bytes;
}
TextResult failure(const QString &diagnostic)
{
    return {std::nullopt, diagnostic};
}
} // namespace

std::optional<TextFormat> resultFormat(const TextFormat &launch, const QByteArray &workingTreeEncoding,
                                      const QByteArray &eol, QString &diagnostic)
{
    auto format = launch;
    if (!absent(workingTreeEncoding)) {
        const auto name = encodingName(workingTreeEncoding);
        if (name == "UTF8") {
            format.encoding = TextEncoding::Utf8;
            format.bom = launch.encoding == TextEncoding::Utf8 && launch.bom;
        } else if (name == "UTF16") {
            format.encoding = launch.encoding == TextEncoding::Utf16BigEndian
                ? TextEncoding::Utf16BigEndian : TextEncoding::Utf16LittleEndian;
            format.bom = true;
        } else if (name == "UTF16LE" || name == "UTF16BE") {
            format.encoding = name == "UTF16LE" ? TextEncoding::Utf16LittleEndian : TextEncoding::Utf16BigEndian;
            format.bom = false; // Git forbids BOMs with explicit UTF-16 byte order.
        } else {
            diagnostic = QStringLiteral("Outside app: unsupported working-tree-encoding: ") + QString::fromLatin1(workingTreeEncoding);
            return std::nullopt;
        }
    }
    if (!absent(eol)) {
        if (eol == "lf") format.newline = Newline::Lf;
        else if (eol == "crlf") format.newline = Newline::CrLf;
        else {
            diagnostic = QStringLiteral("Outside app: unsupported eol attribute");
            return std::nullopt;
        }
    }
    if ((format.encoding != TextEncoding::Utf8 && format.encoding != TextEncoding::Utf16LittleEndian
         && format.encoding != TextEncoding::Utf16BigEndian)
        || (format.newline != Newline::Lf && format.newline != Newline::CrLf)) {
        diagnostic = QStringLiteral("Outside app: invalid result format metadata");
        return std::nullopt;
    }
    return format;
}

TextResult decodeText(const QByteArray &bytes, const QByteArray &workingTreeEncoding)
{
    TextFormat format;
    QString text;
    auto content = bytes;
    const auto name = absent(workingTreeEncoding) ? QByteArray("UTF8") : encodingName(workingTreeEncoding);
    if (name == "UTF8") {
        if (content.startsWith(QByteArray::fromHex("efbbbf"))) {
            format.bom = true;
            content.remove(0, 3);
        }
        text = QString::fromUtf8(content);
        if (text.toUtf8() != content) return failure(QStringLiteral("Outside app: invalid UTF-8 or an unattributed unsupported encoding"));
    } else if (name == "UTF16" || name == "UTF16LE" || name == "UTF16BE") {
        const bool littleBom = content.startsWith(QByteArray::fromHex("fffe"));
        const bool bigBom = content.startsWith(QByteArray::fromHex("feff"));
        if (name == "UTF16" && !littleBom && !bigBom) return failure(QStringLiteral("Outside app: UTF-16 requires a BOM"));
        if ((name == "UTF16LE" && bigBom) || (name == "UTF16BE" && littleBom)) {
            return failure(QStringLiteral("Outside app: UTF-16 BOM contradicts its encoding"));
        }
        format.encoding = name == "UTF16BE" || (name == "UTF16" && bigBom)
            ? TextEncoding::Utf16BigEndian : TextEncoding::Utf16LittleEndian;
        format.bom = littleBom || bigBom;
        if (format.bom) content.remove(0, 2);
        if (content.size() % 2 != 0) return failure(QStringLiteral("Outside app: truncated UTF-16 code unit"));
        std::u16string units;
        for (qsizetype i = 0; i < content.size(); i += 2) {
            const auto a = static_cast<unsigned char>(content[i]);
            const auto b = static_cast<unsigned char>(content[i + 1]);
            units.push_back(static_cast<char16_t>(format.encoding == TextEncoding::Utf16LittleEndian ? a | (b << 8) : (a << 8) | b));
        }
        for (std::size_t i = 0; i < units.size(); ++i) {
            if (QChar::isHighSurrogate(units[i])) {
                if (++i == units.size() || !QChar::isLowSurrogate(units[i])) return failure(QStringLiteral("Outside app: invalid UTF-16 surrogate pair"));
            } else if (QChar::isLowSurrogate(units[i])) return failure(QStringLiteral("Outside app: unpaired UTF-16 surrogate"));
        }
        text = QString::fromUtf16(units.data(), static_cast<qsizetype>(units.size()));
    } else {
        return failure(QStringLiteral("Outside app: unsupported working-tree-encoding: ") + QString::fromLatin1(workingTreeEncoding));
    }
    if (!textCharacters(text)) return failure(QStringLiteral("Outside app: binary/control characters are unsupported"));
    bool lf = false;
    bool crlf = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('\r')) {
            if (i + 1 == text.size() || text[++i] != QLatin1Char('\n')) return failure(QStringLiteral("Outside app: bare CR newlines are unsupported"));
            crlf = true;
        } else if (text[i] == QLatin1Char('\n')) lf = true;
    }
    if (lf && crlf) return failure(QStringLiteral("Outside app: mixed LF and CRLF newlines are unsupported"));
    format.newline = crlf ? Newline::CrLf : Newline::Lf;
    format.finalNewline = text.endsWith(QLatin1Char('\n'));
    if (encode(text, format) != bytes) return failure(QStringLiteral("Outside app: text decoding does not round-trip"));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return {TextDocument{text.toUtf8(), bytes, format}, {}};
}

TextResult serializeText(const QByteArray &utf8, const TextFormat &format)
{
    QString diagnostic;
    if (!resultFormat(format, {}, {}, diagnostic)) return failure(diagnostic);
    auto text = QString::fromUtf8(utf8);
    if (text.toUtf8() != utf8 || !textCharacters(text) || text.contains(QLatin1Char('\r'))) {
        return failure(QStringLiteral("Outside app: editable text must be valid UTF-8 with LF separators and no binary/control characters"));
    }
    if (format.newline == Newline::CrLf) text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const auto bytes = encode(text, format);
    const auto encoding = format.encoding == TextEncoding::Utf8 ? QByteArray("UTF-8")
        : format.encoding == TextEncoding::Utf16LittleEndian ? QByteArray("UTF-16LE") : QByteArray("UTF-16BE");
    auto decoded = decodeText(bytes, encoding);
    if (!decoded.document || decoded.document->utf8 != utf8 || decoded.document->format.bom != format.bom) {
        return failure(QStringLiteral("Outside app: serialized text or BOM does not round-trip"));
    }
    // A file without line separators still retains the selected newline policy.
    decoded.document->format.newline = format.newline;
    return decoded;
}
} // namespace branch_truce::storage
