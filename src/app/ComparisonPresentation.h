// Joined, revision-bound source choices and result section presentation.
#pragma once
#include <QHash>
#include <QObject>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

namespace branch_truce {
class FileSession;
QString correspondenceIdentity(const FileSession &file);
struct ComparisonContribution {
    Q_GADGET
    QML_VALUE_TYPE(comparisonContribution)
#define CONTRIBUTION_PROPERTY(type, name) Q_PROPERTY(type name MEMBER name)
    CONTRIBUTION_PROPERTY(QString, id)
    CONTRIBUTION_PROPERTY(QString, side)
    CONTRIBUTION_PROPERTY(QString, scope)
    CONTRIBUTION_PROPERTY(QString, section)
    CONTRIBUTION_PROPERTY(QString, state)
    CONTRIBUTION_PROPERTY(QString, decisionState)
    CONTRIBUTION_PROPERTY(QStringList, decisionIds)
    CONTRIBUTION_PROPERTY(QString, label)
    CONTRIBUTION_PROPERTY(QString, detail)
    CONTRIBUTION_PROPERTY(bool, available)
    CONTRIBUTION_PROPERTY(bool, combinationAvailable)
    CONTRIBUTION_PROPERTY(bool, handEdited)
    CONTRIBUTION_PROPERTY(bool, markersRemain)
    CONTRIBUTION_PROPERTY(bool, mapped)
    CONTRIBUTION_PROPERTY(int, sourceLineBegin)
    CONTRIBUTION_PROPERTY(int, sourceLineEnd)
    CONTRIBUTION_PROPERTY(int, sourceUtf16Begin)
    CONTRIBUTION_PROPERTY(int, sourceUtf16End)
    CONTRIBUTION_PROPERTY(int, resultLineBegin)
    CONTRIBUTION_PROPERTY(int, resultLineEnd)
    CONTRIBUTION_PROPERTY(int, resultUtf16Begin)
    CONTRIBUTION_PROPERTY(int, resultUtf16End)
#undef CONTRIBUTION_PROPERTY
    Q_PROPERTY(bool undecided READ undecided)
public:
    QString id, side, scope, section, state, decisionState, label, detail;
    QStringList decisionIds;
    bool available = false, combinationAvailable = false, handEdited = false, markersRemain = false, mapped = false;
    int sourceLineBegin = -1, sourceLineEnd = -1, sourceUtf16Begin = -1, sourceUtf16End = -1;
    int resultLineBegin = -1, resultLineEnd = -1, resultUtf16Begin = -1, resultUtf16End = -1;
    bool undecided() const { return state == QLatin1String("undecided"); }
    bool operator==(const ComparisonContribution &) const = default;
};
struct ComparisonRow {
    Q_GADGET
    QML_VALUE_TYPE(comparisonRow)
    Q_PROPERTY(int line MEMBER line)
    Q_PROPERTY(int end MEMBER end)
    Q_PROPERTY(branch_truce::ComparisonContribution choice MEMBER choice)
    Q_PROPERTY(QList<branch_truce::ComparisonContribution> extras MEMBER extras)
public:
    int line = 0, end = 0;
    ComparisonContribution choice;
    QList<ComparisonContribution> extras;
    bool operator==(const ComparisonRow &) const = default;
};
class ComparisonPresentation : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the selected file")
    Q_PROPERTY(QList<branch_truce::ComparisonContribution> contributions READ contributions NOTIFY changed)
    Q_PROPERTY(QList<branch_truce::ComparisonRow> leftRows READ leftRows NOTIFY leftRowsChanged)
    Q_PROPERTY(QList<branch_truce::ComparisonRow> rightRows READ rightRows NOTIFY rightRowsChanged)
#define PRESENTATION_PROPERTY(type, name) Q_PROPERTY(type name READ name NOTIFY changed)
    PRESENTATION_PROPERTY(QVariantList, passages)
    PRESENTATION_PROPERTY(QVariantList, handEditedLines)
    PRESENTATION_PROPERTY(QVariantList, markerBlocks)
    PRESENTATION_PROPERTY(QVariantList, markerOrphans)
    PRESENTATION_PROPERTY(QString, choiceNotice)
    PRESENTATION_PROPERTY(QString, fileId)
    PRESENTATION_PROPERTY(QString, revision)
    PRESENTATION_PROPERTY(QString, correspondence)
    PRESENTATION_PROPERTY(QString, generation)
#undef PRESENTATION_PROPERTY
public:
    using QObject::QObject;
    struct Data {
        QList<ComparisonContribution> contributions;
        QVariantList passages, handEditedLines, markerBlocks, markerOrphans;
        QString choiceNotice, fileId, revision, correspondence;
    };
    // Install every field before emitting any notifier. Rows retain identity
    // across result revisions when their observable source commands are equal.
    void publish(Data data);
#define PRESENTATION_READ(type, name)                                                                                  \
    type name() const { return data_.name; }
    PRESENTATION_READ(QList<ComparisonContribution>, contributions)
    PRESENTATION_READ(QVariantList, passages)
    PRESENTATION_READ(QVariantList, handEditedLines)
    PRESENTATION_READ(QVariantList, markerBlocks)
    PRESENTATION_READ(QVariantList, markerOrphans)
    PRESENTATION_READ(QString, choiceNotice)
    PRESENTATION_READ(QString, fileId)
    PRESENTATION_READ(QString, revision)
    PRESENTATION_READ(QString, correspondence)
#undef PRESENTATION_READ
    QList<ComparisonRow> leftRows() const { return leftRows_; }
    QList<ComparisonRow> rightRows() const { return rightRows_; }
    QString generation() const { return QString::number(generation_); }
    Q_INVOKABLE QVariantMap choiceTarget(const QString &id) const;
    Q_INVOKABLE QVariantMap targetAt(const QString &side, int line) const;
    Q_INVOKABLE QVariantMap activeConflict(int line, const QVariantMap &navigation, const QString &generation) const;
    Q_INVOKABLE QVariantMap resultSections(bool markersShown) const;
    Q_INVOKABLE bool accepts(const QVariantMap &target) const;
Q_SIGNALS:
    void changed();
    void leftRowsChanged();
    void rightRowsChanged();

private:
    struct Section {
        QString id;
        ComparisonContribution left, right;
        int begin = -1, end = -1;
        bool marked = false;
    };
    Data data_;
    quint64 generation_ = 0;
    QHash<QString, ComparisonContribution> byId_;
    QList<ComparisonRow> leftRows_, rightRows_;
    QList<Section> sections_;
};
}
