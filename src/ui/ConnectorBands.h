// Computes connector passages and their visible geometry for a pair of panes.
#pragma once
#include "TextBlocks.h"
#include <QObject>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

namespace branch_truce {
class ConnectorBands : public QObject {
    Q_OBJECT
    QML_ELEMENT
#define BAND_PROPERTY(type, name, Name) \
    Q_PROPERTY(type name READ name WRITE set##Name NOTIFY inputsChanged)
    BAND_PROPERTY(QString, side, Side)
    BAND_PROPERTY(QVariantList, passages, Passages)
    BAND_PROPERTY(QVariantList, regions, Regions)
    BAND_PROPERTY(QVariantList, markerBlocks, MarkerBlocks)
    BAND_PROPERTY(QVariantList, current, Current)
    BAND_PROPERTY(QVariantMap, fold, Fold)
    BAND_PROPERTY(qreal, sourceParentY, SourceParentY)
    BAND_PROPERTY(qreal, resultParentY, ResultParentY)
    BAND_PROPERTY(qreal, sourceEditorY, SourceEditorY)
    BAND_PROPERTY(qreal, resultEditorY, ResultEditorY)
    BAND_PROPERTY(qreal, sourcePadding, SourcePadding)
    BAND_PROPERTY(qreal, resultPadding, ResultPadding)
    BAND_PROPERTY(qreal, sourceScroll, SourceScroll)
    BAND_PROPERTY(qreal, resultScroll, ResultScroll)
    BAND_PROPERTY(qreal, viewportHeight, ViewportHeight)
    BAND_PROPERTY(qreal, rule, Rule)
#undef BAND_PROPERTY
    Q_PROPERTY(TextBlocks *sourceBlocks READ sourceBlocks WRITE setSourceBlocks NOTIFY inputsChanged)
    Q_PROPERTY(TextBlocks *resultBlocks READ resultBlocks WRITE setResultBlocks NOTIFY inputsChanged)
    Q_PROPERTY(QVariantList bands READ bands NOTIFY changed)
    Q_PROPERTY(QVariantList visibleBands READ visibleBands NOTIFY changed)
public:
    explicit ConnectorBands(QObject *parent = nullptr);
#define BAND_ACCESSORS(type, name, Name) \
    type name() const { return name##_; } \
    void set##Name(const type &value) { if (name##_ == value) return; name##_ = value; projectionDirty_ = true; update(); Q_EMIT inputsChanged(); }
    BAND_ACCESSORS(QString, side, Side)
    BAND_ACCESSORS(QVariantList, passages, Passages)
    BAND_ACCESSORS(QVariantList, regions, Regions)
    BAND_ACCESSORS(QVariantList, markerBlocks, MarkerBlocks)
    BAND_ACCESSORS(QVariantMap, fold, Fold)
#undef BAND_ACCESSORS
#define BAND_REAL(name, Name) \
    qreal name() const { return name##_; } \
    void set##Name(qreal value) { if (qFuzzyCompare(name##_, value)) return; name##_ = value; update(); Q_EMIT inputsChanged(); }
    BAND_REAL(sourceParentY, SourceParentY)
    BAND_REAL(resultParentY, ResultParentY)
    BAND_REAL(sourceEditorY, SourceEditorY)
    BAND_REAL(resultEditorY, ResultEditorY)
    BAND_REAL(sourcePadding, SourcePadding)
    BAND_REAL(resultPadding, ResultPadding)
    BAND_REAL(sourceScroll, SourceScroll)
    BAND_REAL(resultScroll, ResultScroll)
    BAND_REAL(viewportHeight, ViewportHeight)
    BAND_REAL(rule, Rule)
#undef BAND_REAL
    QVariantList current() const { return current_; }
    void setCurrent(const QVariantList &value) { if (current_ == value) return; current_ = value; update(); Q_EMIT inputsChanged(); }
    TextBlocks *sourceBlocks() const { return sourceBlocks_; }
    TextBlocks *resultBlocks() const { return resultBlocks_; }
    void setSourceBlocks(TextBlocks *blocks);
    void setResultBlocks(TextBlocks *blocks);
    Q_INVOKABLE QVariantMap bandForLine(int line, bool result) const;
    Q_INVOKABLE QVariantMap bandAt(qreal x, qreal y, qreal width, bool leftSide) const;
    QVariantList bands() const { return bands_; }
    QVariantList visibleBands() const { return visibleBands_; }
Q_SIGNALS:
    void inputsChanged();
    void changed();
private:
    void update();
    QString side_;
    QVariantList passages_, regions_, markerBlocks_, current_, bands_, visibleBands_;
    QVariantMap fold_;
    bool projectionDirty_ = true;
    qreal sourceParentY_ = 0, resultParentY_ = 0, sourceEditorY_ = 0, resultEditorY_ = 0;
    qreal sourcePadding_ = 0, resultPadding_ = 0, sourceScroll_ = 0, resultScroll_ = 0;
    qreal viewportHeight_ = 0, rule_ = 0;
    QPointer<TextBlocks> sourceBlocks_, resultBlocks_;
    QMetaObject::Connection sourceConnection_, resultConnection_;
};
}
