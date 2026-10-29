// Formatting for captured source roles and object identities.
#pragma once
#include <QCoreApplication>
#include <QString>

namespace branch_truce {
inline QString shortId(const QString &id) { return id.left(10); }

inline QString sourceRole(QString label, const QString &commitId = {})
{
    // Older recovery records can contain the commit as part of the label.
    if (!commitId.isEmpty() && label.endsWith(QStringLiteral(": ") + commitId))
        label.chop(commitId.size() + 2);
    if (label == QLatin1String("Current checkout")) return QCoreApplication::translate("SourceIdentity", "Current checkout");
    if (label == QLatin1String("Merged commit")) return QCoreApplication::translate("SourceIdentity", "Merged commit");
    if (label == QLatin1String("Current rebased state")) return QCoreApplication::translate("SourceIdentity", "Current rebased state");
    if (label == QLatin1String("Commit being replayed")) return QCoreApplication::translate("SourceIdentity", "Commit being replayed");
    if (label == QLatin1String("Common earlier version")) return QCoreApplication::translate("SourceIdentity", "Common earlier version");
    return label;
}

inline QString sourceIdentity(const QString &label, const QString &id)
{
    const auto role = sourceRole(label, id);
    return id.isEmpty() ? role : QCoreApplication::translate("SourceIdentity", "%1: %2").arg(role, shortId(id));
}
}
