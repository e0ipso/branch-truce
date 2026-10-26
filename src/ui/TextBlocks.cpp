#include "TextBlocks.h"
#include <QAbstractTextDocumentLayout>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>

namespace branch_truce {
TextBlocks::TextBlocks(QObject *parent) : QObject(parent) {}

QQuickTextDocument *TextBlocks::document() const { return document_; }

void TextBlocks::setDocument(QQuickTextDocument *document)
{
    if (document_ == document) return;
    for (const auto &connection : std::as_const(connections_)) disconnect(connection);
    connections_.clear();
    document_ = document;
    if (auto *text = document ? document->textDocument() : nullptr) {
        connections_.append(connect(text, &QTextDocument::contentsChanged, this, &TextBlocks::layoutChanged));
        connections_.append(connect(text, &QTextDocument::blockCountChanged, this, &TextBlocks::blockCountChanged));
        if (auto *layout = text->documentLayout())
            connections_.append(connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged,
                this, &TextBlocks::layoutChanged));
    }
    Q_EMIT documentChanged();
    Q_EMIT blockCountChanged();
    Q_EMIT layoutChanged();
}

int TextBlocks::blockCount() const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    return text ? text->blockCount() : 0;
}

QVariantList TextBlocks::between(qreal top, qreal bottom) const
{
    QVariantList blocks;
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout || bottom <= top) return blocks;
    const int first = layout->hitTest(QPointF(0, std::max<qreal>(0, top)), Qt::FuzzyHit);
    for (auto block = text->findBlock(std::max(0, first)); block.isValid(); block = block.next()) {
        const auto rect = layout->blockBoundingRect(block);
        if (rect.top() >= bottom) break;
        if (rect.bottom() <= top) continue;
        blocks.append(QVariantMap{{QStringLiteral("number"), block.blockNumber() + 1},
            {QStringLiteral("position"), block.position()}});
    }
    return blocks;
}
}
