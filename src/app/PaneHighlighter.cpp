#include "PaneHighlighter.h"
#include <QTextDocument>

namespace branch_truce {
PaneHighlighter::PaneHighlighter(QTextDocument *document)
    : PaneHighlighterBase(static_cast<QObject *>(document))
{
}

void PaneHighlighter::attach()
{
    auto *target = qobject_cast<QTextDocument *>(parent());
    if (document() != target) setDocument(target);
}

#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
void PaneHighlighter::highlight(const KSyntaxHighlighting::Definition &definition,
    const KSyntaxHighlighting::Theme &theme)
{
    // setDefinition rehighlights an attached document when the definition
    // changes; setTheme never does.
    const bool recolour = theme.name() != this->theme().name();
    const bool redefine = definition != this->definition();
    if (recolour) setTheme(theme);
    setDefinition(definition);
    if (document() != parent()) attach();
    else if (recolour && !redefine && definition.isValid()) rehighlight();
}
#endif

void PaneHighlighter::setDecorations(const QList<QTextLayout::FormatRange> &ranges)
{
    if (ranges == decorations_) return;
    decorations_ = ranges;
    if (document()) rehighlight();
}

void PaneHighlighter::highlightBlock(const QString &text)
{
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
    if (definition().isValid()) SyntaxHighlighter::highlightBlock(text);
#else
    Q_UNUSED(text)
#endif
    // The decoration pass goes here, after the syntax formats.
}
}
