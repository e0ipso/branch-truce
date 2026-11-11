// Syntax highlighting selects language definitions and theme colours for comparison documents.
#include "highlighting/SyntaxHighlighting.h"
#include "highlighting/PaneHighlighter.h"
#include <KSyntaxHighlighting/Repository>
#include <QList>
#include <QPointer>
#include <QTextDocument>

namespace branch_truce {
struct SyntaxHighlighting::Private {
    // Loading definitions and themes is costly, so one repository serves the controller's lifetime.
    KSyntaxHighlighting::Repository repository;
    KSyntaxHighlighting::Definition definition;
    KSyntaxHighlighting::Theme theme = repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme);
    QString fileName;
    QList<QPointer<PaneHighlighter>> highlighters;

    // A document can outlive the controller. Its highlighter must not outlive the repository.
    ~Private() { qDeleteAll(highlighters); }
    void apply()
    {
        highlighters.removeAll(nullptr);
        for (const auto &highlighter : std::as_const(highlighters)) highlighter->highlight(definition, theme);
    }
};

SyntaxHighlighting::SyntaxHighlighting() : d_(std::make_unique<Private>()) {}

SyntaxHighlighting::~SyntaxHighlighting() = default;

void SyntaxHighlighting::attach(QTextDocument *document)
{
    if (!document || document->findChild<PaneHighlighter *>(QString(), Qt::FindDirectChildrenOnly)) return;
    auto *highlighter = new PaneHighlighter(document);
    // Destroyed documents take their highlighters along; drop them here as the plain-text build does.
    d_->highlighters.removeAll(nullptr);
    d_->highlighters.push_back(highlighter);
    highlighter->highlight(d_->definition, d_->theme);
}

void SyntaxHighlighting::setFileName(const QString &fileName)
{
    if (fileName == d_->fileName) return;
    d_->fileName = fileName;
    d_->definition =
        fileName.isEmpty() ? KSyntaxHighlighting::Definition{} : d_->repository.definitionForFileName(fileName);
    d_->apply();
}

void SyntaxHighlighting::setBackground(const QColor &background)
{
    if (!background.isValid()) return;
    const auto theme =
        d_->repository.defaultTheme(background.lightnessF() < 0.5 ? KSyntaxHighlighting::Repository::DarkTheme
                                                                  : KSyntaxHighlighting::Repository::LightTheme);
    if (theme.name() == d_->theme.name()) return;
    d_->theme = theme;
    d_->apply();
}
}
