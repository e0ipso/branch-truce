// Outside-app text formats safe staging guidance for a file the app cannot edit.
#pragma once

#include <QByteArray>
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

    Q_INVOKABLE QString gitAddCommand(const QByteArray &path) const
    {
        if (path.isEmpty()) return {};
        const auto text = QString::fromUtf8(path);
        if (text.toUtf8() != path) return {};
        // An escaped display path is not a shell operand. Leave uncopyable byte paths
        // and control characters to another tool rather than present a different path.
        for (const auto value : text)
            if (value.category() == QChar::Other_Control || value.category() == QChar::Other_Format
                || value == QChar::LineSeparator || value == QChar::ParagraphSeparator)
                return {};
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
