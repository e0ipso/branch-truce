// The plain-text build retains comparison decorations while leaving syntax colours disabled.
#include "highlighting/SyntaxHighlighting.h"
#include "highlighting/PaneHighlighter.h"
#include <QList>
#include <QPointer>
#include <QTextDocument>

namespace branch_truce {
// Built with BRANCH_TRUCE_SYNTAX_HIGHLIGHTING=OFF: the panes show plain text,
// and each document still gets a PaneHighlighter for its decorations.
struct SyntaxHighlighting::Private {
    QList<QPointer<PaneHighlighter>> highlighters;

    // Matches the highlighting build: no highlighter outlives the controller.
    ~Private() { qDeleteAll(highlighters); }
};

SyntaxHighlighting::SyntaxHighlighting() : d_(std::make_unique<Private>()) {}

SyntaxHighlighting::~SyntaxHighlighting() = default;

void SyntaxHighlighting::attach(QTextDocument *document)
{
    if (!document || document->findChild<PaneHighlighter *>(QString(), Qt::FindDirectChildrenOnly)) return;
    auto *highlighter = new PaneHighlighter(document);
    d_->highlighters.removeAll(nullptr);
    d_->highlighters.push_back(highlighter);
    highlighter->attach();
}

void SyntaxHighlighting::setFileName(const QString &) {}

void SyntaxHighlighting::setBackground(const QColor &) {}
}
