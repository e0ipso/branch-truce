// Qt text conversions turn engine byte strings, paths, revisions and documents into the strings views show.
#pragma once
#include "core/RevisionId.h"
#include <QString>
#include <QTextDocument>
#include <string>

namespace branch_truce::qt_text {
// Engine text is UTF-8.
[[nodiscard]] inline QString utf8(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// The undecorated last path component, for syntax definition lookup.
[[nodiscard]] inline QString baseName(const std::string &path) { return utf8(path.substr(path.rfind('/') + 1)); }

// Decimal strings retain all 64 bits across the QML/JavaScript boundary.
[[nodiscard]] inline QString revision(RevisionId id) { return QString::number(static_cast<qulonglong>(id)); }

// A document's text with Qt's paragraph separators back as line feeds.
[[nodiscard]] inline QString documentText(const QTextDocument &document)
{
    return document.toRawText().replace(QChar(0x2029), QLatin1Char('\n'));
}
}
