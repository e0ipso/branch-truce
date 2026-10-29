// Computes comparison pane line marks, folds and navigation ranges.
#pragma once
#include "TextBlocks.h"
#include <QObject>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
class PaneMarks : public QObject {
    Q_OBJECT
    QML_ELEMENT
public:
    using QObject::QObject;
    static int rank(const QString &kind);
    Q_INVOKABLE QVariantMap marks(const QString &pane, const QVariantList &passages,
        const QVariantList &regions, const QVariantList &edited, const QVariantList &blocks,
        const QVariantList &orphans) const;
    Q_INVOKABLE QVariantMap folds(const QVariantList &blocks, bool shown) const;
    Q_INVOKABLE QVariantList current(const QString &pane, const QVariant &target,
        const QString &fileId, const QVariantList &passages, const QVariantList &regions,
        TextBlocks *blocks) const;
    Q_INVOKABLE bool contains(const QVariantList &ranges, int line) const;
    Q_INVOKABLE bool gap(const QVariantList &ranges, int line) const;
    Q_INVOKABLE bool overlaps(const QVariantList &ranges, int first, int last) const;
};
}
