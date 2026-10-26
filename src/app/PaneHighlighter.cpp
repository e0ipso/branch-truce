#include "PaneHighlighter.h"
#include <QTextDocument>

namespace branch_truce {
// Parent only: the document is set once there is a definition to apply.
PaneHighlighter::PaneHighlighter(QTextDocument *document)
    : SyntaxHighlighter(static_cast<QObject *>(document))
{
}

void PaneHighlighter::highlight(const KSyntaxHighlighting::Definition &definition,
    const KSyntaxHighlighting::Theme &theme)
{
    if (!definition.isValid()) {
        setDocument(nullptr);
        setDefinition(definition);
        return;
    }
    // setDefinition rehighlights an attached document when the definition
    // changes; setTheme never does. Attaching highlights on the next event loop turn.
    const bool recolour = theme.name() != this->theme().name();
    const bool redefine = definition != this->definition();
    if (recolour) setTheme(theme);
    setDefinition(definition);
    auto *target = qobject_cast<QTextDocument *>(parent());
    if (document() != target) setDocument(target);
    else if (recolour && !redefine) rehighlight();
}
}
