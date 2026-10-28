#include "storage/TextFormat.h"
#include "TestHarness.h"
#include <array>

using namespace branch_truce::storage;

namespace {
struct EncodingCase {
    TextEncoding encoding;
    bool bom;
    QByteArray attribute;
    QByteArray prefix;
    QByteArray line;
    QByteArray lf;
    QByteArray cr;
    QByteArray tab;
};

const std::array<EncodingCase, 6> encodings{{
    {TextEncoding::Utf8, false, {}, {}, QByteArray::fromHex("41c3a9f09f9982"), "\n", "\r", "\t"},
    {TextEncoding::Utf8, true, "uTf8", QByteArray::fromHex("efbbbf"),
        QByteArray::fromHex("41c3a9f09f9982"), "\n", "\r", "\t"},
    {TextEncoding::Utf16LittleEndian, false, "utf16le", {}, QByteArray::fromHex("4100e9003dd842de"),
        QByteArray::fromHex("0a00"), QByteArray::fromHex("0d00"), QByteArray::fromHex("0900")},
    {TextEncoding::Utf16BigEndian, false, "uTf-16be", {}, QByteArray::fromHex("004100e9d83dde42"),
        QByteArray::fromHex("000a"), QByteArray::fromHex("000d"), QByteArray::fromHex("0009")},
    {TextEncoding::Utf16LittleEndian, true, "UTF-16", QByteArray::fromHex("fffe"),
        QByteArray::fromHex("4100e9003dd842de"), QByteArray::fromHex("0a00"),
        QByteArray::fromHex("0d00"), QByteArray::fromHex("0900")},
    {TextEncoding::Utf16BigEndian, true, "UTF16", QByteArray::fromHex("feff"),
        QByteArray::fromHex("004100e9d83dde42"), QByteArray::fromHex("000a"),
        QByteArray::fromHex("000d"), QByteArray::fromHex("0009")}
}};
}

class TextFormatTests {
public:
    int runTests()
    {
        test_support::Suite suite("TextFormatTests");
        suite.run("supported encodings preserve Unicode, BOM, separators and final newline", [] {
            const auto line = QByteArray::fromHex("41c3a9f09f9982");
            for (const auto &encoding : encodings) {
                for (const auto newline : {Newline::Lf, Newline::CrLf}) {
                    for (const bool final : {false, true}) {
                        const auto separator = (newline == Newline::CrLf ? encoding.cr : QByteArray{}) + encoding.lf;
                        const auto bytes = encoding.prefix + encoding.line + separator + encoding.tab + encoding.line
                            + (final ? separator : QByteArray{});
                        const auto editable = line + "\n\t" + line + (final ? "\n" : "");
                        const TextFormat format{encoding.encoding, encoding.bom, newline, final};
                        const auto decoded = decodeText(bytes, encoding.attribute);
                        CHECK(decoded.document && decoded.diagnostic.isEmpty());
                        CHECK(decoded.document->utf8 == editable && decoded.document->originalBytes == bytes);
                        CHECK(decoded.document->format == format);
                        const auto serialized = serializeText(editable, format);
                        CHECK(serialized.document && serialized.diagnostic.isEmpty());
                        CHECK(serialized.document->originalBytes == bytes && serialized.document->format == format);
                    }
                }
            }
        });
        suite.run("empty files and lines without separators do not acquire a final newline", [] {
            const auto line = QByteArray::fromHex("41c3a9f09f9982");
            for (const auto &encoding : encodings) {
                for (const bool empty : {false, true}) {
                    const auto editable = empty ? QByteArray{} : line;
                    const auto bytes = encoding.prefix + (empty ? QByteArray{} : encoding.line);
                    const auto decoded = decodeText(bytes, encoding.attribute);
                    CHECK(decoded.document && decoded.document->utf8 == editable);
                    CHECK(!decoded.document->format.finalNewline && decoded.document->originalBytes == bytes);
                    // Stale launch metadata cannot add a newline to an edited buffer.
                    const auto serialized = serializeText(editable,
                        {encoding.encoding, encoding.bom, Newline::CrLf, true});
                    CHECK(serialized.document && serialized.document->originalBytes == bytes);
                    CHECK(!serialized.document->format.finalNewline);
                    CHECK(serialized.document->format.newline == Newline::CrLf);
                }
            }
        });
        suite.run("invalid encodings, controls and ambiguous newlines have explicit rejection reasons", [] {
            struct Invalid { QByteArray bytes; QByteArray encoding; };
            const Invalid cases[]{
                {QByteArray::fromHex("ff"), {}}, {QByteArray::fromHex("c0af"), {}},
                {QByteArray::fromHex("e282"), {}}, {QByteArray::fromHex("eda080"), {}},
                {QByteArray::fromHex("410042"), {}}, {QByteArray::fromHex("01"), {}},
                {QByteArray::fromHex("7f"), {}}, {"a\rb", {}}, {"a\r\nb\n", {}},
                {QByteArray::fromHex("4100"), "UTF-16"},
                {QByteArray::fromHex("fffe41"), "UTF-16"},
                {QByteArray::fromHex("00d8"), "UTF-16LE"},
                {QByteArray::fromHex("00dc"), "UTF-16LE"},
                {QByteArray::fromHex("00d84100"), "UTF-16LE"},
                {QByteArray::fromHex("feff0041"), "UTF-16LE"},
                {QByteArray::fromHex("fffe4100"), "UTF-16BE"},
                {"plain", "ISO-8859-1"}
            };
            for (const auto &invalid : cases) {
                const auto decoded = decodeText(invalid.bytes, invalid.encoding);
                CHECK(!decoded.document && !decoded.diagnostic.isEmpty());
            }
        });
        suite.run("serialization accepts only editable UTF-8 and takes final newline from the buffer", [] {
            for (const auto &invalid : {QByteArray::fromHex("ff"), QByteArray::fromHex("610062"),
                    QByteArray("a\rb"), QByteArray("a\r\n"), QByteArray::fromHex("efbbbf61")}) {
                const auto result = serializeText(invalid, {});
                CHECK(!result.document && !result.diagnostic.isEmpty());
            }
            const auto added = serializeText("line\n", {TextEncoding::Utf8, false, Newline::CrLf, false});
            CHECK(added.document && added.document->originalBytes == "line\r\n");
            CHECK(added.document->format.finalNewline);
            const auto removed = serializeText("line", {TextEncoding::Utf8, true, Newline::Lf, true});
            CHECK(removed.document && removed.document->originalBytes == QByteArray::fromHex("efbbbf") + "line");
            CHECK(!removed.document->format.finalNewline);
        });
        suite.run("explicit encoding and EOL override launch format with deterministic BOM rules", [] {
            const TextFormat launch{TextEncoding::Utf16BigEndian, true, Newline::CrLf, true};
            struct Selection { QByteArray encoding; QByteArray eol; TextFormat expected; };
            const Selection cases[]{
                {{}, {}, launch}, {"unset", "unspecified", launch},
                {"UTF8", "lf", {TextEncoding::Utf8, false, Newline::Lf, true}},
                {"UTF-16", {}, launch},
                {"utf16le", "lf", {TextEncoding::Utf16LittleEndian, false, Newline::Lf, true}},
                {"utf-16be", "crlf", {TextEncoding::Utf16BigEndian, false, Newline::CrLf, true}}
            };
            for (const auto &selection : cases) {
                QString diagnostic;
                const auto format = resultFormat(launch, selection.encoding, selection.eol, diagnostic);
                CHECK(format && *format == selection.expected && diagnostic.isEmpty());
            }
            QString diagnostic;
            const TextFormat utf8Bom{TextEncoding::Utf8, true, Newline::Lf, false};
            CHECK(resultFormat(utf8Bom, "UTF-8", {}, diagnostic) == utf8Bom);
            const auto utf16 = resultFormat(utf8Bom, "UTF-16", {}, diagnostic);
            CHECK(utf16 && utf16->encoding == TextEncoding::Utf16LittleEndian && utf16->bom);
            CHECK(!resultFormat(launch, "UTF-32", {}, diagnostic) && !diagnostic.isEmpty());
            CHECK(!resultFormat(launch, {}, "native", diagnostic) && !diagnostic.isEmpty());
            CHECK(!resultFormat({static_cast<TextEncoding>(99), false, Newline::Lf, false}, {}, {}, diagnostic));
            CHECK(!resultFormat({TextEncoding::Utf8, false, static_cast<Newline>(99), false}, {}, {}, diagnostic));
        });
        return suite.report();
    }
};

int main()
{
    return TextFormatTests{}.runTests();
}
