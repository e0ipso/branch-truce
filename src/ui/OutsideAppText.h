#pragma once

#include <QByteArray>
#include <QMimeDatabase>
#include <QMimeType>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
// Display-only formatting of captured facts. It reads no selected file and runs no command.
class OutsideAppText : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit OutsideAppText(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE bool binaryExcluded(const QString &support, const QString &detail, const QByteArray &path) const
    {
        if (support != QLatin1String("outside-app")) return false;
        // Only exact decoder diagnostics establish text rejection. A MIME extension alone
        // must not turn an I/O, topology or conversion-policy failure into a binary claim.
        static const QRegularExpression decoderReason(
            QStringLiteral("\\AOutside app: (?:stage [123] cannot be edited: )?"
                           "(binary/control characters are unsupported|"
                           "invalid UTF-8 or an unattributed unsupported encoding)\\z"));
        const auto reason = decoderReason.match(detail);
        if (!reason.hasMatch()) return false;
        if (reason.captured(1) == QLatin1String("binary/control characters are unsupported")) return true;
        const auto name = QString::fromUtf8(path);
        if (name.toUtf8() != path) return false;
        const auto mime = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
        // Invalid UTF-8 in a text or unknown type does not prove that it is binary.
        return mime.isValid() && !mime.isDefault() && !mime.inherits(QStringLiteral("text/plain"));
    }

    Q_INVOKABLE QString gitAddCommand(const QByteArray &path) const
    {
        if (path.isEmpty()) return {};
        const auto text = QString::fromUtf8(path);
        if (text.toUtf8() != path) return {};
        // An escaped display path is not a shell operand. Leave uncopyable byte paths
        // and control characters to another tool rather than present a different path.
        for (const auto value : text)
            if (value.category() == QChar::Other_Control || value.category() == QChar::Other_Format
                || value == QChar::LineSeparator || value == QChar::ParagraphSeparator) return {};
        static const QRegularExpression simplePath(QStringLiteral("\\A[A-Za-z0-9_./][A-Za-z0-9_./-]*\\z"));
        if (simplePath.match(text).hasMatch()) return QStringLiteral("git add ") + text;
        auto quoted = text;
        quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        // POSIX single quoting protects shell syntax. Literal pathspecs and -- also
        // protect Git glob/magic syntax and leading dashes, independently of the shell.
        return QStringLiteral("git --literal-pathspecs add -- '") + quoted + QLatin1Char('\'');
    }
};
}
