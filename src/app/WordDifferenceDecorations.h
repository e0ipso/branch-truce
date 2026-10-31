// Word difference decorations highlight changed words without changing the result text.
#pragma once
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTextLayout>
#include <QtGlobal>
#include <functional>
#include <map>
#include <utility>

class QTextDocument;

namespace branch_truce {
class CorrespondenceMap;
class SessionModel;
struct FileSnapshots;

// Draws the word-level differences of the selected file's current revision
// (fileWordDifferences) in each attached pane, through the pane document's
// PaneHighlighter. A range gets the line's theme role as a translucent
// background and an underline, so the cue survives without colour. The text
// of every line marked ignored takes the disabled colour, since an ignored
// side has no background fill. It only decorates: no text, decision or
// revision changes.
//
// Each pane editor declares wordDifferenceStyle, an object with:
//   pane      "stage2", "stage3", "earlier" or "result"
//   lines     the line kinds its backgrounds draw, keyed by 1-based line
//   positive, active, negative, neutral, disabled
//             the Kirigami.Theme text colours of those roles
// A pane draws word ranges only while its text is exactly the text they were
// computed for; otherwise it shows none.
class WordDifferenceDecorations final : public QObject {
    Q_OBJECT
public:
    WordDifferenceDecorations(SessionModel &files, std::function<QString()> selectedFileId,
                              QObject *parent = nullptr);
    // Attaching the same editor again changes nothing. An editor without
    // wordDifferenceStyle, or without a highlighter yet, is left alone.
    void attach(QObject *editor, QTextDocument *document);

public Q_SLOTS:
    // Computes the ranges when the selected file or its revision changed, and
    // redraws every pane whose text, line kinds or colours changed.
    void refresh();

private:
    struct Pane {
        QPointer<QObject> editor;
        QPointer<QTextDocument> document;
        QString name;
        // The text check is repeated only after the document changed or
        // the ranges are for other text.
        bool dirty = true;
        QString checkedFor;
        bool matches = false;
    };
    struct PaneRanges {
        QString text;
        // UTF-16 [start, length) in text.
        QList<std::pair<int, int>> ranges;
    };
    void compute(const QString &fileId);
    // ranges is null when the pane shows no word ranges.
    [[nodiscard]] QList<QTextLayout::FormatRange> formats(const Pane &pane, const PaneRanges *ranges) const;

    SessionModel &files_;
    std::function<QString()> selectedFileId_;
    QList<Pane> panes_;
    // What the ranges were computed from.
    QString fileId_;
    quint64 revision_ = 0;
    const CorrespondenceMap *mapping_ = nullptr;
    const FileSnapshots *captures_ = nullptr;
    std::map<QString, PaneRanges> ranges_;
    bool refreshing_ = false;
};
}
