// Computes comparison pane line marks, folds and navigation ranges.
#pragma once
#include "TextBlocks.h"
#include "app/ComparisonPresentation.h"
#include <QObject>
#include <QSet>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
class PaneMarks : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(branch_truce::ComparisonPresentation *presentation READ presentation WRITE setPresentation NOTIFY
                   presentationChanged)
    Q_PROPERTY(bool markersShown READ markersShown WRITE setMarkersShown NOTIFY markersShownChanged)
#define MARKS_PROPERTY(name) Q_PROPERTY(QVariantMap name READ name NOTIFY name##Changed)
    MARKS_PROPERTY(lineMarks)
    MARKS_PROPERTY(fold)
    MARKS_PROPERTY(sections)
#undef MARKS_PROPERTY
public:
    explicit PaneMarks(QObject *parent = nullptr) : QObject(parent) { refresh(); }
    ComparisonPresentation *presentation() const { return presentation_; }
    void setPresentation(ComparisonPresentation *comparison);
    bool markersShown() const { return markersShown_; }
    void setMarkersShown(bool shown);
#define MARKS_READ(name)                                                                                               \
    QVariantMap name() const { return name##_; }
    MARKS_READ(lineMarks)
    MARKS_READ(fold)
    MARKS_READ(sections)
#undef MARKS_READ
    static int rank(const QString &kind);
    static QVariantList decoratedPassages(const QVariantList &passages);
    static QList<ComparisonContribution> stylingRegions(const QList<ComparisonContribution> &contributions);
    static QSet<int> conflictingLines(const QVariantList &passages);
    static bool conflictingRegion(const ComparisonContribution &region, const QSet<int> &lines);
    Q_INVOKABLE QVariantMap marks(const QString &pane, const QVariantList &passages,
                                  const QList<ComparisonContribution> &contributions, const QVariantList &edited,
                                  const QVariantList &blocks, const QVariantList &orphans) const;
    Q_INVOKABLE QVariantMap folds(const QVariantList &blocks, bool shown) const;
    Q_INVOKABLE QVariantList current(const QString &pane, const QVariant &target, const QString &fileId,
                                     const QVariantList &passages, const QList<ComparisonContribution> &contributions,
                                     TextBlocks *blocks) const;
Q_SIGNALS:
    void presentationChanged();
    void markersShownChanged();
#define MARKS_SIGNAL(name) void name##Changed();
    MARKS_SIGNAL(lineMarks)
    MARKS_SIGNAL(fold)
    MARKS_SIGNAL(sections)
#undef MARKS_SIGNAL
public:
    Q_INVOKABLE bool contains(const QVariantList &ranges, int line) const;
    Q_INVOKABLE bool gap(const QVariantList &ranges, int line) const;
    Q_INVOKABLE bool overlaps(const QVariantList &ranges, int first, int last) const;

private:
    void refresh();
    QPointer<ComparisonPresentation> presentation_;
    QMetaObject::Connection presentationConnection_;
    bool markersShown_ = false;
    QVariantMap lineMarks_, fold_, sections_;
};
}
