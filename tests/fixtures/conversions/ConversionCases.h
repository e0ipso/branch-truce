#pragma once

#include "TextConversion.h"

namespace conversion_fixtures {
using namespace branch_truce::storage;

inline const QByteArray bom = QByteArray::fromHex("efbbbf");
inline const QByteArray unicode = QByteArray::fromHex("41c3a9f09f9982"); // A, é, 🙂
inline const QByteArray unicodeLe = QByteArray::fromHex("4100e9003dd842de");
inline const QByteArray unicodeBe = QByteArray::fromHex("004100e9d83dde42");
inline const TextFormat lf{TextEncoding::Utf8, false, Newline::Lf, true};
inline const TextFormat crlf{TextEncoding::Utf8, false, Newline::CrLf, true};
inline const TextFormat bomLf{TextEncoding::Utf8, true, Newline::Lf, true};
inline const TextFormat bomCrLf{TextEncoding::Utf8, true, Newline::CrLf, true};
inline const TextFormat leLf{TextEncoding::Utf16LittleEndian, false, Newline::Lf, true};
inline const TextFormat beCrLf{TextEncoding::Utf16BigEndian, false, Newline::CrLf, true};
inline const TextFormat leBomLf{TextEncoding::Utf16LittleEndian, true, Newline::Lf, true};
inline const TextFormat beBomCrLf{TextEncoding::Utf16BigEndian, true, Newline::CrLf, true};

// All byte expectations are literal data. No production codec builds the oracle.
struct FormatCase {
    const char *name;
    QByteArray attributes;
    QStringList configuration;
    QByteArray canonicalSource;
    QByteArray checkoutSource;
    TextFormat launch;
    QByteArray checkoutResult;
    QByteArray canonicalResult;
    TextFormat resultFormat;
    QByteArray displayedSource = "s\n";
    QByteArray editedUtf8 = "r\n";
    QByteArray mode = "100644";
};

inline const FormatCase accepted[]{
    {"unattributed LF", {}, {}, "s\n", "s\n", lf, "r\n", "r\n", lf},
    {"unattributed CRLF", {}, {}, "s\r\n", "s\r\n", crlf, "r\r\n", "r\r\n", crlf},
    {"UTF-8 BOM", {}, {}, bom + "s\n", bom + "s\n", bomCrLf, bom + "r\r\n", bom + "r\r\n", bomCrLf},
    {"explicit LF overrides launch", "text eol=lf", {}, "s\n", "s\n", crlf, "r\n", "r\n", lf},
    {"auto text with explicit CRLF", "text=auto eol=crlf", {}, "s\n", "s\r\n", lf, "r\r\n", "r\n", crlf},
    {"disabled text conversion", "-text", {}, "s\r\n", "s\r\n", crlf, "r\r\n", "r\r\n", crlf},
    {"explicit UTF-8 retains BOM", "text eol=crlf working-tree-encoding=utf-8", {},
        bom + "s\n", bom + "s\r\n", bomCrLf, bom + "r\r\n", bom + "r\n", bomCrLf},
    {"UTF-16LE without BOM", "text eol=lf working-tree-encoding=UTF-16LE", {}, "s\n",
        QByteArray::fromHex("73000a00"), bomCrLf, QByteArray::fromHex("72000a00"), "r\n", leLf},
    {"UTF-16BE with CRLF", "text eol=crlf working-tree-encoding=UTF-16BE", {}, "s\n",
        QByteArray::fromHex("0073000d000a"), lf, QByteArray::fromHex("0072000d000a"), "r\n", beCrLf},
    {"UTF-16 default byte order and BOM", "text eol=lf working-tree-encoding=UTF-16", {}, "s\n",
        QByteArray::fromHex("fffe73000a00"), bomCrLf, QByteArray::fromHex("fffe72000a00"), "r\n", leBomLf},
    {"autocrlf with explicit CRLF", "text eol=crlf", {"core.autocrlf", "true"},
        "s\n", "s\r\n", lf, "r\r\n", "r\n", crlf},
    {"core EOL conversion", "text", {"core.eol", "crlf"}, "s\n", "s\r\n", crlf, "r\r\n", "r\n", crlf},
    {"autocrlf input", "text=auto", {"core.autocrlf", "input"}, "s\n", "s\n", lf, "r\n", "r\n", lf},
    {"disabled text overrides autocrlf", "-text", {"core.autocrlf", "true"},
        "s\r\n", "s\r\n", crlf, "r\r\n", "r\r\n", crlf},
    {"Unicode UTF-8 without final newline", {}, {}, unicode + "\n\t" + unicode, unicode + "\n\t" + unicode,
        lf, unicode + "\n\t" + unicode + '!', unicode + "\n\t" + unicode + '!',
        {TextEncoding::Utf8, false, Newline::Lf, false}, unicode + "\n\t" + unicode,
        unicode + "\n\t" + unicode + '!', "100755"},
    {"Unicode UTF-8 BOM and CRLF without final newline", "text eol=crlf working-tree-encoding=UTF-8", {},
        bom + unicode + "\n\t" + unicode, bom + unicode + "\r\n\t" + unicode, bomLf,
        bom + unicode + "\r\n\t" + unicode + '!', bom + unicode + "\n\t" + unicode + '!',
        {TextEncoding::Utf8, true, Newline::CrLf, false}, unicode + "\n\t" + unicode,
        unicode + "\n\t" + unicode + '!'},
    {"Unicode UTF-16LE without final newline", "text eol=lf working-tree-encoding=UTF-16LE", {},
        unicode + "\n\t" + unicode, unicodeLe + QByteArray::fromHex("0a000900") + unicodeLe, crlf,
        unicodeLe + QByteArray::fromHex("0a000900") + unicodeLe + QByteArray::fromHex("2100"),
        unicode + "\n\t" + unicode + '!', {TextEncoding::Utf16LittleEndian, false, Newline::Lf, false},
        unicode + "\n\t" + unicode, unicode + "\n\t" + unicode + '!'},
    {"Unicode UTF-16BE with CRLF", "text eol=crlf working-tree-encoding=UTF-16BE", {},
        unicode + '\n', unicodeBe + QByteArray::fromHex("000d000a"), bomCrLf,
        unicodeBe + QByteArray::fromHex("0021000d000a"), unicode + "!\n", beCrLf,
        unicode + '\n', unicode + "!\n", "100755"},
    {"Unicode UTF-16 with BOM", "text eol=lf working-tree-encoding=UTF-16", {},
        unicode + '\n', QByteArray::fromHex("fffe") + unicodeLe + QByteArray::fromHex("0a00"), bomCrLf,
        QByteArray::fromHex("fffe") + unicodeLe + QByteArray::fromHex("21000a00"), unicode + "!\n", leBomLf,
        unicode + '\n', unicode + "!\n", "100755"},
    {"explicit LE replaces big-endian launch BOM", "text eol=lf working-tree-encoding=UTF-16LE", {},
        unicode + '\n', unicodeLe + QByteArray::fromHex("0a00"), beBomCrLf,
        unicodeLe + QByteArray::fromHex("21000a00"), unicode + "!\n", leLf, unicode + '\n', unicode + "!\n"},
    {"explicit UTF-8 replaces UTF-16 launch", "text eol=lf working-tree-encoding=UTF-8", {},
        unicode + '\n', unicode + '\n', beBomCrLf, unicode + "!\n", unicode + "!\n", lf,
        unicode + '\n', unicode + "!\n"},
    {"empty UTF-8 does not acquire a newline", "text eol=crlf", {}, {}, {}, lf, {}, {},
        {TextEncoding::Utf8, false, Newline::CrLf, false}, {}, {}},
    {"BOM-only UTF-8 does not acquire a newline", "text eol=lf", {}, bom, bom, bomCrLf, bom, bom,
        {TextEncoding::Utf8, true, Newline::Lf, false}, {}, {}},
    {"empty UTF-16LE does not acquire a newline", "text eol=lf working-tree-encoding=UTF-16LE", {},
        {}, {}, crlf, {}, {}, {TextEncoding::Utf16LittleEndian, false, Newline::Lf, false}, {}, {}},
    {"edited final newline overrides launch metadata", {}, {}, "s", "s", {}, "r\n", "r\n", lf, "s"},
    {"CRLF source does not choose the result format", {}, {}, "s\r\n", "s\r\n", lf, "r\n", "r\n", lf}
};

struct RejectedSource {
    const char *name;
    QByteArray canonical;
    QByteArray attributes;
    branch_truce::git::TextConversionFailure failure = branch_truce::git::TextConversionFailure::UnsupportedText;
};
inline const RejectedSource rejectedSources[]{
    {"invalid UTF-8 byte", QByteArray::fromHex("ff"), {}},
    {"overlong UTF-8", QByteArray::fromHex("c0af"), {}},
    {"truncated UTF-8", QByteArray::fromHex("e282"), {}},
    {"UTF-8 surrogate", QByteArray::fromHex("eda080"), {}},
    {"NUL", QByteArray::fromHex("610062"), {}},
    {"binary control", QByteArray::fromHex("610162"), {}},
    {"DEL", QByteArray::fromHex("617f62"), {}},
    {"bare CR", "a\rb", {}},
    {"mixed LF and CRLF", "a\r\nb\n", {}},
    {"noncanonical CRLF under LF attributes", "s\r\n", "text eol=lf",
        branch_truce::git::TextConversionFailure::UnsupportedConversion}
};

inline const char *rejectedAttributes[]{
    "-text eol=crlf", "text=custom", "ident=custom", "text eol=native",
    "working-tree-encoding=ISO-8859-1", "working-tree-encoding=UTF-32"
};

struct RejectedEdit { const char *name; QByteArray bytes; };
inline const RejectedEdit rejectedEdits[]{
    {"invalid UTF-8", QByteArray::fromHex("ff")},
    {"overlong UTF-8", QByteArray::fromHex("c0af")},
    {"truncated UTF-8", QByteArray::fromHex("e282")},
    {"UTF-8 surrogate", QByteArray::fromHex("eda080")},
    {"NUL", QByteArray::fromHex("610062")},
    {"binary control", QByteArray::fromHex("610162")},
    {"bare CR", "a\rb"},
    {"CRLF in editable text", "a\r\n"},
    {"mixed separators in editable text", "a\r\nb\n"},
    {"BOM in editable text", bom + "a"}
};
} // namespace conversion_fixtures
