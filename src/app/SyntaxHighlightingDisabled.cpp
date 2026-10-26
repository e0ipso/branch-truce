#include "SyntaxHighlighting.h"

namespace branch_truce {
// Built with BRANCH_TRUCE_SYNTAX_HIGHLIGHTING=OFF: the panes show plain text.
struct SyntaxHighlighting::Private {};

SyntaxHighlighting::SyntaxHighlighting() = default;

SyntaxHighlighting::~SyntaxHighlighting() = default;

void SyntaxHighlighting::attach(QTextDocument *) {}

void SyntaxHighlighting::setFileName(const QString &) {}

void SyntaxHighlighting::setBackground(const QColor &) {}
}
