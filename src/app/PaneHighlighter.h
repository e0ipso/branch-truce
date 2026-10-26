#pragma once
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

class QTextDocument;

namespace branch_truce {
// The single owner of character formats on one pane document, and its child.
// highlightBlock applies the syntax formats; nothing else may call setFormat
// or set a QTextCharFormat on the document. Format changes leave the text
// alone, so the result editor's edit capture ignores them.
class PaneHighlighter final : public KSyntaxHighlighting::SyntaxHighlighter {
    Q_OBJECT
public:
    explicit PaneHighlighter(QTextDocument *document);
    // Formats the document with definition in theme. Without a valid
    // definition it detaches from the document, which clears its formats and
    // skips the per-block work on files it cannot highlight.
    void highlight(const KSyntaxHighlighting::Definition &definition, const KSyntaxHighlighting::Theme &theme);
};
}
