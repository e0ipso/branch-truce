#include "SyntaxHighlighting.h"
#include "PaneHighlighter.h"
#include <KSyntaxHighlighting/Repository>
#include <QList>
#include <QPointer>
#include <QTextDocument>

namespace branch_truce {
struct SyntaxHighlighting::Private {
    // Loading definitions and themes is costly, so one repository serves the controller's lifetime.
    KSyntaxHighlighting::Repository repository;
    KSyntaxHighlighting::Definition definition;
    KSyntaxHighlighting::Theme theme = repository.theme(QStringLiteral("Breeze Light"));
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
    d_->highlighters.push_back(highlighter);
    highlighter->highlight(d_->definition, d_->theme);
}

void SyntaxHighlighting::setFileName(const QString &fileName)
{
    if (fileName == d_->fileName) return;
    d_->fileName = fileName;
    d_->definition = fileName.isEmpty() ? KSyntaxHighlighting::Definition{}
                                        : d_->repository.definitionForFileName(fileName);
    d_->apply();
}

void SyntaxHighlighting::setBackground(const QColor &background)
{
    if (!background.isValid()) return;
    const auto name = background.lightnessF() < 0.5 ? QStringLiteral("Breeze Dark") : QStringLiteral("Breeze Light");
    if (name == d_->theme.name()) return;
    d_->theme = d_->repository.theme(name);
    d_->apply();
}
}
