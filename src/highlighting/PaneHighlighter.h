#pragma once
#include <QList>
#include <QTextLayout>
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#else
#include <QSyntaxHighlighter>
#endif

class QTextDocument;

namespace branch_truce {
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
using PaneHighlighterBase = KSyntaxHighlighting::SyntaxHighlighter;
#else
using PaneHighlighterBase = QSyntaxHighlighter;
#endif

// The single owner of character formats on one pane document, and its child.
// highlightBlock applies the syntax formats, then the decorations; nothing else
// may call setFormat or set a QTextCharFormat on the document. Format changes
// leave the text alone, so the result editor's edit capture ignores them. It
// stays attached whether or not a definition matches, and in builds without
// syntax highlighting, so decorations reach every file.
class PaneHighlighter final : public PaneHighlighterBase {
    Q_OBJECT
public:
    // Parent only: attach() or highlight() sets the document.
    explicit PaneHighlighter(QTextDocument *document);
    // Attaches to the parent document. It highlights on the next event loop turn.
    void attach();
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
    // Formats the document with definition in theme and attaches to it.
    // Without a valid definition the blocks get decorations only.
    void highlight(const KSyntaxHighlighting::Definition &definition, const KSyntaxHighlighting::Theme &theme);
#endif
    // Ranges in document positions, kept in position order, merged over the
    // syntax formats of the characters they cover. Replacing them runs a
    // format pass over the blocks the old or new ranges touch.
    void setDecorations(const QList<QTextLayout::FormatRange> &ranges);
    const QList<QTextLayout::FormatRange> &decorations() const { return decorations_; }
    // Search matches share this highlighter with syntax and word differences.
    void setSearchDecorations(const QList<QTextLayout::FormatRange> &ranges);

protected:
    void highlightBlock(const QString &text) override;

private:
    void decorate(int begin, int end);
    void updateDecorations();
    QList<QTextLayout::FormatRange> wordDecorations_;
    QList<QTextLayout::FormatRange> searchDecorations_;
    QList<QTextLayout::FormatRange> decorations_;
    // reach_[i] is the furthest end of decorations_[0..i], so a binary search
    // finds the first range that can reach a block.
    QList<int> reach_;
};
}
