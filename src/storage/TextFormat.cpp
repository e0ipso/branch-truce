// Text formatting decodes captured files and serializes results with their retained encoding and line endings.
#include "TextFormat.h"
#include "core/Utf8.h"

#include <string>
#include <utility>

namespace branch_truce::storage {
namespace {
using Encoding = TextEncoding;

QByteArray normalizedName(QByteArray name)
{
    name = name.toUpper();
    name.replace("-", "");
    return name;
}
bool absent(const QByteArray &value) { return value.isEmpty() || value == "unspecified" || value == "unset"; }
QByteArray encode(const QString &text, const TextFormat &format)
{
    if (format.encoding == Encoding::Utf8) {
        return (format.bom ? QByteArray::fromHex("efbbbf") : QByteArray{}) + text.toUtf8();
    }
    QByteArray bytes;
    if (format.bom) bytes = QByteArray::fromHex(format.encoding == Encoding::Utf16LittleEndian ? "fffe" : "feff");
    for (const auto character : text) {
        const auto value = static_cast<unsigned int>(character.unicode());
        const char low = static_cast<char>(value & 0xffU);
        const char high = static_cast<char>(value >> 8U);
        if (format.encoding == Encoding::Utf16LittleEndian) {
            bytes += low;
            bytes += high;
        } else {
            bytes += high;
            bytes += low;
        }
    }
    return bytes;
}
Problem decodeUtf16(QByteArray content, const QByteArray &name, TextFormat &format, QString &text)
{
    const bool littleBom = content.startsWith(QByteArray::fromHex("fffe"));
    const bool bigBom = content.startsWith(QByteArray::fromHex("feff"));
    if (name == "UTF16" && !littleBom && !bigBom) return Problem(ProblemCode::TextMissingByteOrderMark);
    if ((name == "UTF16LE" && bigBom) || (name == "UTF16BE" && littleBom)) {
        return Problem(ProblemCode::TextContradictoryByteOrderMark);
    }
    format.encoding =
        name == "UTF16BE" || (name == "UTF16" && bigBom) ? Encoding::Utf16BigEndian : Encoding::Utf16LittleEndian;
    format.bom = littleBom || bigBom;
    if (format.bom) content.remove(0, 2);
    if (content.size() % 2 != 0) return Problem(ProblemCode::TextTruncatedUtf16);
    std::u16string units;
    units.reserve(static_cast<std::size_t>(content.size() / 2));
    for (qsizetype i = 0; i < content.size(); i += 2) {
        const auto a = static_cast<unsigned int>(static_cast<unsigned char>(content[i]));
        const auto b = static_cast<unsigned int>(static_cast<unsigned char>(content[i + 1]));
        units.push_back(
            static_cast<char16_t>(format.encoding == Encoding::Utf16LittleEndian ? a | (b << 8U) : (a << 8U) | b));
    }
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (QChar::isHighSurrogate(units[i])) {
            ++i;
            if (i == units.size() || !QChar::isLowSurrogate(units[i]))
                return Problem(ProblemCode::TextInvalidSurrogatePair);
        } else if (QChar::isLowSurrogate(units[i]))
            return Problem(ProblemCode::TextUnpairedSurrogate);
    }
    text = QString::fromUtf16(units.data(), static_cast<qsizetype>(units.size()));
    return {};
}
TextResult failure(Problem problem) { return {std::nullopt, std::move(problem)}; }
} // namespace

QByteArray encodingName(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf16LittleEndian: return QByteArrayLiteral("UTF-16LE");
    case Encoding::Utf16BigEndian: return QByteArrayLiteral("UTF-16BE");
    case Encoding::Utf8: break;
    }
    return QByteArrayLiteral("UTF-8");
}

std::optional<TextFormat> resultFormat(const TextFormat &launch, const QByteArray &workingTreeEncoding,
                                       const QByteArray &eol, Problem &problem)
{
    auto format = launch;
    if (!absent(workingTreeEncoding)) {
        const auto name = normalizedName(workingTreeEncoding);
        if (name == "UTF8") {
            format.encoding = Encoding::Utf8;
            format.bom = launch.encoding == Encoding::Utf8 && launch.bom;
        } else if (name == "UTF16") {
            format.encoding =
                launch.encoding == Encoding::Utf16BigEndian ? Encoding::Utf16BigEndian : Encoding::Utf16LittleEndian;
            format.bom = true;
        } else if (name == "UTF16LE" || name == "UTF16BE") {
            format.encoding = name == "UTF16LE" ? Encoding::Utf16LittleEndian : Encoding::Utf16BigEndian;
            format.bom = false; // Git forbids BOMs with explicit UTF-16 byte order.
        } else {
            problem = {ProblemCode::TextUnsupportedEncoding, {QString::fromLatin1(workingTreeEncoding)}};
            return std::nullopt;
        }
    }
    if (!absent(eol)) {
        if (eol == "lf")
            format.newline = Newline::Lf;
        else if (eol == "crlf")
            format.newline = Newline::CrLf;
        else {
            problem = ProblemCode::TextUnsupportedEol;
            return std::nullopt;
        }
    }
    return format;
}

TextResult decodeText(const QByteArray &bytes, const QByteArray &workingTreeEncoding)
{
    TextFormat format;
    QString text;
    auto content = bytes;
    const auto name = absent(workingTreeEncoding) ? QByteArray("UTF8") : normalizedName(workingTreeEncoding);
    if (name == "UTF8") {
        if (content.startsWith(QByteArray::fromHex("efbbbf"))) {
            format.bom = true;
            content.remove(0, 3);
        }
        text = QString::fromUtf8(content);
        if (text.toUtf8() != content) return failure(ProblemCode::TextInvalidUtf8);
    } else if (name == "UTF16" || name == "UTF16LE" || name == "UTF16BE") {
        if (const auto problem = decodeUtf16(std::move(content), name, format, text); !problem.isEmpty())
            return failure(problem);
    } else {
        return failure(Problem{ProblemCode::TextUnsupportedEncoding, {QString::fromLatin1(workingTreeEncoding)}});
    }
    bool lf = false;
    bool crlf = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('\r')) {
            ++i;
            if (i == text.size() || text[i] != QLatin1Char('\n')) return failure(ProblemCode::TextBareCarriageReturn);
            crlf = true;
        } else if (text[i] == QLatin1Char('\n'))
            lf = true;
    }
    if (lf && crlf) return failure(ProblemCode::TextMixedNewlines);
    format.newline = crlf ? Newline::CrLf : Newline::Lf;
    format.finalNewline = text.endsWith(QLatin1Char('\n'));
    if (encode(text, format) != bytes) return failure(ProblemCode::TextNoRoundTrip);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const auto utf8 = text.toUtf8();
    if (!editableText(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size()))))
        return failure(ProblemCode::TextControlCharacters);
    return {TextDocument{utf8, bytes, format}, {}};
}

TextResult serializeText(const QByteArray &utf8, const TextFormat &format)
{
    auto text = QString::fromUtf8(utf8);
    if (!editableText(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())))) {
        return failure(ProblemCode::TextInvalidEditable);
    }
    if (format.newline == Newline::CrLf) text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    const auto bytes = encode(text, format);
    auto decoded = decodeText(bytes, encodingName(format.encoding));
    if (!decoded.document || decoded.document->utf8 != utf8 || decoded.document->format.bom != format.bom) {
        return failure(ProblemCode::TextSerializedNoRoundTrip);
    }
    // A file without line separators still retains the selected newline policy.
    decoded.document->format.newline = format.newline;
    return decoded;
}
} // namespace branch_truce::storage
