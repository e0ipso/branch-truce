#pragma once
#include <QColor>
#include <QString>
#include <memory>

class QTextDocument;

namespace branch_truce {
// Syntax colours for the comparison panes. Each attached document gets one
// PaneHighlighter, the only code that sets character formats on it. Line
// backgrounds and whitespace markers are separate QML layers. A build with
// BRANCH_TRUCE_SYNTAX_HIGHLIGHTING=OFF attaches highlighters that apply no
// syntax formats and ignores the file name and background.
class SyntaxHighlighting {
public:
    SyntaxHighlighting();
    ~SyntaxHighlighting();
    SyntaxHighlighting(const SyntaxHighlighting &) = delete;
    SyntaxHighlighting &operator=(const SyntaxHighlighting &) = delete;
    // Highlights document with the current definition and theme until the
    // document is destroyed. Attaching it again changes nothing.
    void attach(QTextDocument *document);
    // The definition comes from the file name. A name with no definition
    // leaves every attached document without syntax formats.
    void setFileName(const QString &fileName);
    // Breeze Dark on a dark scheme background, Breeze Light otherwise.
    void setBackground(const QColor &background);

private:
    struct Private;
    std::unique_ptr<Private> d_;
};
}
