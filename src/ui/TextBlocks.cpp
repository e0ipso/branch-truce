#include "TextBlocks.h"
#include <QAbstractTextDocumentLayout>
#include <QQuickTextDocument>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <algorithm>

namespace branch_truce {
namespace {
QTextBlock shown(QTextBlock block, bool forward)
{
    while (block.isValid() && !block.isVisible()) block = forward ? block.next() : block.previous();
    return block;
}
}

TextBlocks::TextBlocks(QObject *parent) : QObject(parent) {}

QQuickTextDocument *TextBlocks::document() const { return document_; }

void TextBlocks::setDocument(QQuickTextDocument *document)
{
    if (document_ == document) return;
    for (const auto &connection : std::as_const(connections_)) disconnect(connection);
    connections_.clear();
    document_ = document;
    hiddenCount_ = 0;
    if (auto *text = document ? document->textDocument() : nullptr) {
        connections_.append(connect(text, &QTextDocument::contentsChanged, this, &TextBlocks::layoutChanged));
        connections_.append(connect(text, &QTextDocument::blockCountChanged, this, &TextBlocks::blockCountChanged));
        connections_.append(connect(text, &QTextDocument::contentsChange, this, &TextBlocks::scheduleHiding));
        if (auto *layout = text->documentLayout())
            connections_.append(connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged,
                this, &TextBlocks::layoutChanged));
        for (auto block = text->begin(); block.isValid(); block = block.next())
            hiddenCount_ += !block.isVisible();
    }
    Q_EMIT documentChanged();
    Q_EMIT blockCountChanged();
    applyHidden();
    Q_EMIT layoutChanged();
}

int TextBlocks::blockCount() const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    return text ? text->blockCount() : 0;
}

QVariantList TextBlocks::hidden() const { return hidden_; }

void TextBlocks::setHidden(const QVariantList &hidden)
{
    QHash<int, QString> lines;
    for (const auto &entry : hidden) {
        const auto map = entry.toMap();
        const int line = map.value(QStringLiteral("line")).toInt();
        if (line > 0) lines.insert(line, map.value(QStringLiteral("text")).toString());
    }
    hidden_ = hidden;
    hiddenText_ = lines;
    applyHidden();
    Q_EMIT hiddenChanged();
}

// Replaced text has new blocks, all shown, and an edit can join a hidden line
// to its neighbour. Apply the hidden lines again once the change is complete,
// rather than editing the document while it reports the change.
void TextBlocks::scheduleHiding()
{
    if (applying_ || hidingQueued_ || (hiddenText_.isEmpty() && hiddenCount_ == 0)) return;
    hidingQueued_ = true;
    QMetaObject::invokeMethod(this, [this] {
        if (hidingQueued_) applyHidden();
    }, Qt::QueuedConnection);
}

void TextBlocks::applyHidden()
{
    hidingQueued_ = false;
    auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text || (hiddenText_.isEmpty() && hiddenCount_ == 0)) return;
    QScopedValueRollback guard(applying_, true);
    QList<QTextBlock> changed;
    int count = 0;
    for (auto block = text->begin(); block.isValid(); block = block.next()) {
        const auto wanted = hiddenText_.constFind(block.blockNumber() + 1);
        const bool show = wanted == hiddenText_.cend() || block.text() != *wanted;
        count += !show;
        if (block.isVisible() == show) continue;
        block.setVisible(show);
        changed.append(block);
    }
    hiddenCount_ = count;
    if (changed.isEmpty()) return;
    // Setting a block's own format leaves its text alone, but the change
    // makes the layout and the TextEdit lay out and draw the block again.
    QTextCursor cursor(text);
    cursor.beginEditBlock();
    for (const auto &block : std::as_const(changed)) {
        cursor.setPosition(block.position());
        cursor.setBlockFormat(cursor.blockFormat());
    }
    cursor.endEditBlock();
}

// A hidden block has no layout, so it takes no height at the top of the next
// shown block, or below the last one.
QRectF TextBlocks::extent(QAbstractTextDocumentLayout *layout, const QTextBlock &block) const
{
    if (block.isVisible()) return layout->blockBoundingRect(block);
    if (const auto next = shown(block, true); next.isValid()) {
        const auto rect = layout->blockBoundingRect(next);
        return {rect.left(), rect.top(), rect.width(), 0};
    }
    if (const auto previous = shown(block, false); previous.isValid()) {
        const auto rect = layout->blockBoundingRect(previous);
        return {rect.left(), rect.bottom(), rect.width(), 0};
    }
    return {};
}

QVariantList TextBlocks::between(qreal top, qreal bottom) const
{
    QVariantList blocks;
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout || bottom <= top) return blocks;
    const int first = layout->hitTest(QPointF(0, std::max<qreal>(0, top)), Qt::FuzzyHit);
    for (auto block = text->findBlock(std::max(0, first)); block.isValid(); block = block.next()) {
        if (!block.isVisible()) continue;
        const auto rect = layout->blockBoundingRect(block);
        if (rect.top() >= bottom) break;
        if (rect.bottom() <= top) continue;
        blocks.append(QVariantMap{{QStringLiteral("number"), block.blockNumber() + 1},
            {QStringLiteral("position"), block.position()},
            {QStringLiteral("top"), rect.top()}, {QStringLiteral("height"), rect.height()}});
    }
    return blocks;
}

int TextBlocks::lineAt(int position) const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text) return 0;
    const auto block = text->findBlock(std::clamp(position, 0, std::max(0, text->characterCount() - 1)));
    return block.isValid() ? block.blockNumber() + 1 : 0;
}

qreal TextBlocks::lineAtY(qreal y) const
{
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout || y <= 0) return 1;
    auto block = text->findBlock(std::max(0, layout->hitTest(QPointF(0, y), Qt::FuzzyHit)));
    if (!block.isValid()) block = text->lastBlock();
    if (!block.isVisible()) {
        const auto next = shown(block, true);
        block = next.isValid() ? next : shown(block, false);
    }
    if (!block.isValid()) return 1;
    auto rect = layout->blockBoundingRect(block);
    // A fuzzy hit can land on a neighbouring block at a boundary.
    for (auto previous = shown(block.previous(), false); y < rect.top() && previous.isValid();
         previous = shown(block.previous(), false)) {
        block = previous;
        rect = layout->blockBoundingRect(block);
    }
    for (auto next = shown(block.next(), true); y >= rect.bottom() && next.isValid(); next = shown(block.next(), true)) {
        block = next;
        rect = layout->blockBoundingRect(block);
    }
    const auto part = rect.height() > 0 ? (y - rect.top()) / rect.height() : 0;
    return block.blockNumber() + 1 + std::clamp<qreal>(part, 0, 1);
}

qreal TextBlocks::yAtLine(qreal line) const
{
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout) return 0;
    const auto clamped = std::clamp<qreal>(line, 1, text->blockCount() + 1);
    const int number = std::min(static_cast<int>(clamped), text->blockCount());
    const auto rect = extent(layout, text->findBlockByNumber(number - 1));
    return rect.top() + (clamped - number) * rect.height();
}

int TextBlocks::positionOf(int line) const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text) return 0;
    return text->findBlockByNumber(std::clamp(line, 1, text->blockCount()) - 1).position();
}

bool TextBlocks::hiddenAt(int position) const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text || hiddenCount_ == 0 || position < 0 || position >= text->characterCount()) return false;
    return !text->findBlock(position).isVisible();
}

bool TextBlocks::hiddenWithin(int begin, int end) const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text || hiddenCount_ == 0 || end <= begin) return false;
    for (auto block = text->findBlock(std::max(0, begin)); block.isValid() && block.position() < end;
         block = block.next()) {
        if (!block.isVisible()) return true;
    }
    return false;
}

int TextBlocks::shownPosition(int position, bool forward, int from) const
{
    auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text || !hiddenAt(position)) return position;
    const auto block = text->findBlock(position);
    const auto after = shown(block, true);
    const auto before = shown(block, false);
    const bool down = forward ? after.isValid() : !before.isValid();
    const auto target = down ? after : before;
    if (!target.isValid()) return position;
    const int edge = down ? target.position() : target.position() + target.length() - 1;
    const auto source = from >= 0 ? text->findBlock(from) : QTextBlock{};
    if (!source.isValid() || !source.isVisible() || !source.layout()) return edge;
    const auto sourceLine = source.layout()->lineForTextPosition(from - source.position());
    // Laying out the document gives the target block its lines.
    (void)text->documentLayout()->blockBoundingRect(target);
    auto *layout = target.layout();
    if (!sourceLine.isValid() || !layout || layout->lineCount() == 0) return edge;
    const auto line = layout->lineAt(down ? 0 : layout->lineCount() - 1);
    return target.position() + line.xToCursor(sourceLine.cursorToX(from - source.position()));
}
}
