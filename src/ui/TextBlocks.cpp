#include "TextBlocks.h"
#include <QAbstractTextDocumentLayout>
#include <QQuickTextDocument>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QVariantMap>
#include <algorithm>
#include <utility>

namespace branch_truce {
namespace {
QTextBlock shown(QTextBlock block, bool forward)
{
    while (block.isValid() && !block.isVisible()) block = forward ? block.next() : block.previous();
    return block;
}

// Appends the markers of one shown block whose layout starts at origin.
void appendWhitespace(QVariantList &markers, const QTextBlock &block, QPointF origin, const QRectF &area,
                      qreal stroke, qreal tabWidth)
{
    const auto *lines = block.layout();
    const auto text = block.text();
    for (int row = 0; lines && row < lines->lineCount(); ++row) {
        const auto line = lines->lineAt(row);
        const auto top = origin.y() + line.y();
        if (top >= area.bottom()) break;
        if (top + line.height() < area.top()) continue;
        // Only the characters across the area, with one more at either side.
        const int start = line.textStart();
        const int from = std::max(start, line.xToCursor(area.left() - origin.x()) - 1);
        const int to = std::min(start + line.textLength(), line.xToCursor(area.right() - origin.x()) + 1);
        for (int at = from; at < to; ++at) {
            const bool tab = text.at(at) == QLatin1Char('\t');
            if (!tab && text.at(at) != QLatin1Char(' ')) continue;
            const auto x = origin.x() + line.cursorToX(at);
            auto width = lines->lineForTextPosition(at + 1).lineNumber() == row
                ? origin.x() + line.cursorToX(at + 1) - x : 0;
            if (width < stroke) width = tab ? tabWidth : stroke;
            if (x >= area.right() || x + width <= area.left()) continue;
            markers.append(QVariantMap{{QStringLiteral("x"), x - area.left()}, {QStringLiteral("y"), top - area.top()},
                {QStringLiteral("w"), width}, {QStringLiteral("h"), std::max(line.height(), stroke)},
                {QStringLiteral("tab"), tab}});
        }
    }
}
}

TextBlocks::TextBlocks(QObject *parent) : QObject(parent) {}

QQuickTextDocument *TextBlocks::document() const { return document_; }

void TextBlocks::setDocument(QQuickTextDocument *document)
{
    if (document_ == document) return;
    disconnect(documentSwapConnection_);
    if (text_ && rowSpacing_ > 0) applySpacing(text_, 0, 0, text_->characterCount());
    document_ = document;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    // Qt 6.7 added replacement of the QTextDocument behind the wrapper.
    if (document_)
        documentSwapConnection_ = connect(document_, &QQuickTextDocument::textDocumentChanged,
            this, &TextBlocks::reconnectDocument);
#endif
    reconnectDocument();
    Q_EMIT documentChanged();
}

void TextBlocks::reconnectDocument()
{
    for (const auto &connection : std::as_const(connections_)) disconnect(connection);
    connections_.clear();
    if (text_ && text_ != (document_ ? document_->textDocument() : nullptr) && rowSpacing_ > 0)
        applySpacing(text_, 0, 0, text_->characterCount());
    text_ = document_ ? document_->textDocument() : nullptr;
    spacingFrom_ = spacingTo_ = -1;
    hiddenCount_ = 0;
    rows_.clear();
    if (auto *text = text_.data()) {
        connections_.append(connect(text, &QTextDocument::contentsChanged, this, &TextBlocks::layoutChanged));
        connections_.append(connect(text, &QTextDocument::blockCountChanged, this, &TextBlocks::blockCountChanged));
        connections_.append(connect(text, &QTextDocument::contentsChange, this,
            [this](int position, int removed, int added) {
                scheduleHiding();
                scheduleSpacing(position, removed, added);
            }));
        if (auto *layout = text->documentLayout())
            connections_.append(connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged,
                this, &TextBlocks::layoutChanged));
        for (auto block = text->begin(); block.isValid(); block = block.next())
            hiddenCount_ += !block.isVisible();
        if (rowSpacing_ > 0) {
            // The result uses application history; sources are read-only. Block
            // formatting must never enter the native document undo stack.
            text->setUndoRedoEnabled(false);
            applySpacing(text, rowSpacing_, 0, text->characterCount());
        }
    }
    Q_EMIT blockCountChanged();
    applyHidden();
    Q_EMIT layoutChanged();
}

qreal TextBlocks::rowSpacing() const { return rowSpacing_; }

void TextBlocks::setRowSpacing(qreal spacing)
{
    spacing = std::max<qreal>(0, spacing);
    if (qFuzzyCompare(rowSpacing_ + 1, spacing + 1)) return;
    rowSpacing_ = spacing;
    if (text_) {
        if (spacing > 0) text_->setUndoRedoEnabled(false);
        applySpacing(text_, spacing, 0, text_->characterCount());
    }
    Q_EMIT rowSpacingChanged();
    Q_EMIT layoutChanged();
}

void TextBlocks::scheduleSpacing(int position, int removed, int added)
{
    if (applying_ || rowSpacing_ <= 0 || !text_) return;
    if (spacingFrom_ < 0) {
        spacingFrom_ = position;
        spacingTo_ = position + added;
    } else {
        // Multiple changes before the queued pass may shift earlier ranges.
        // Recheck from the earliest change through the end in that case.
        spacingFrom_ = std::min(spacingFrom_, position);
        spacingTo_ = text_->characterCount();
    }
    Q_UNUSED(removed);
    if (spacingQueued_) return;
    spacingQueued_ = true;
    QMetaObject::invokeMethod(this, [this] {
        spacingQueued_ = false;
        flushSpacing();
    }, Qt::QueuedConnection);
}

void TextBlocks::flushSpacing()
{
    if (applying_) return;
    const int from = std::exchange(spacingFrom_, -1);
    const int to = std::exchange(spacingTo_, -1);
    if (text_ && rowSpacing_ > 0 && from >= 0) applySpacing(text_, rowSpacing_, from, to);
}

void TextBlocks::applySpacing(QTextDocument *text, qreal spacing, int from, int to)
{
    if (!text) return;
    QScopedValueRollback guard(applying_, true);
    const bool modified = text->isModified();
    auto block = text->findBlock(std::clamp(from, 0, text->characterCount() - 1));
    QTextCursor cursor(text);
    cursor.beginEditBlock();
    for (; block.isValid() && block.position() <= to; block = block.next()) {
        auto format = block.blockFormat();
        const bool already = spacing > 0
            ? format.lineHeightType() == QTextBlockFormat::LineDistanceHeight && qFuzzyCompare(format.lineHeight() + 1, spacing + 1)
            : !format.hasProperty(QTextFormat::LineHeight) && !format.hasProperty(QTextFormat::LineHeightType);
        if (already) continue;
        if (spacing > 0) format.setLineHeight(spacing, QTextBlockFormat::LineDistanceHeight);
        else {
            format.clearProperty(QTextFormat::LineHeight);
            format.clearProperty(QTextFormat::LineHeightType);
        }
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
    // Qt reports formatting as modification, although raw text did not change.
    if (text->isModified() != modified) text->setModified(modified);
}

int TextBlocks::blockCount() const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    return text ? text->blockCount() : 0;
}

QVariantList TextBlocks::hidden() const { return hidden_; }

void TextBlocks::setHidden(const QVariantList &hidden)
{
    QList<Fold> folds;
    for (const auto &entry : hidden) {
        const auto map = entry.toMap();
        Fold fold{map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("text")).toString(),
            map.value(QStringLiteral("last")).toInt(), map.value(QStringLiteral("lastText")).toString()};
        if (fold.line <= 0) continue;
        if (fold.last <= fold.line) fold.last = 0;
        folds.append(fold);
    }
    hidden_ = hidden;
    folds_ = folds;
    applyHidden();
    Q_EMIT hiddenChanged();
}

// Replaced text has new blocks, all shown, and an edit can join a hidden line
// to its neighbour. Apply the hidden lines again once the change is complete,
// rather than editing the document while it reports the change.
void TextBlocks::scheduleHiding()
{
    if (applying_ || hidingQueued_ || (folds_.isEmpty() && hiddenCount_ == 0 && rows_.isEmpty())) return;
    hidingQueued_ = true;
    QMetaObject::invokeMethod(this, [this] {
        if (hidingQueued_) applyHidden();
    }, Qt::QueuedConnection);
}

void TextBlocks::applyHidden()
{
    hidingQueued_ = false;
    auto *text = document_ ? document_->textDocument() : nullptr;
    const auto previousRows = std::exchange(rows_, {});
    if (!text || (folds_.isEmpty() && hiddenCount_ == 0 && previousRows.isEmpty())) return;
    QScopedValueRollback guard(applying_, true);
    const auto holds = [text](int line, const QString &wanted) {
        const auto block = text->findBlockByNumber(line - 1);
        return block.isValid() && block.text() == wanted;
    };
    QSet<int> hide;
    for (const auto &fold : std::as_const(folds_)) {
        if (!holds(fold.line, fold.text) || (fold.last > 0 && !holds(fold.last, fold.lastText))) continue;
        if (fold.last == 0) {
            hide.insert(fold.line);
            continue;
        }
        rows_.insert(fold.line);
        for (int line = fold.line + 1; line <= fold.last; ++line) hide.insert(line);
    }
    // A row hidden by another entry is no row.
    rows_.removeIf([&hide](int line) { return hide.contains(line); });
    QList<QTextBlock> changed;
    int count = 0;
    for (auto block = text->begin(); block.isValid(); block = block.next()) {
        const bool show = !hide.contains(block.blockNumber() + 1);
        count += !show;
        const int line = block.blockNumber() + 1;
        if (block.isVisible() == show && rows_.contains(line) == previousRows.contains(line)) continue;
        block.setVisible(show);
        changed.append(block);
    }
    hiddenCount_ = count;
    if (changed.isEmpty()) return;
    // Setting a block's own format leaves its text alone, but the change
    // makes the layout and the TextEdit lay out and draw the block again.
    const bool modified = text->isModified();
    QTextCursor cursor(text);
    cursor.beginEditBlock();
    for (const auto &block : std::as_const(changed)) {
        cursor.setPosition(block.position());
        auto format = cursor.blockFormat();
        // The opening marker may include a long branch name. Its presentation
        // row must stay one line even while ordinary result lines wrap.
        format.setNonBreakableLines(rows_.contains(block.blockNumber() + 1));
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
    if (text->isModified() != modified) text->setModified(modified);
}

// A hidden block has no layout, so it takes no height at the top of the next
// shown block, or below the last one.
QRectF TextBlocks::extent(QAbstractTextDocumentLayout *layout, const QTextBlock &block) const
{
    if (block.isVisible()) {
        auto rect = layout->blockBoundingRect(block);
        const auto format = block.blockFormat();
        if (format.lineHeightType() == QTextBlockFormat::LineDistanceHeight)
            rect.setHeight(rect.height() + format.lineHeight());
        return rect;
    }
    if (const auto next = shown(block, true); next.isValid()) {
        const auto rect = layout->blockBoundingRect(next);
        return {rect.left(), rect.top(), rect.width(), 0};
    }
    if (const auto previous = shown(block, false); previous.isValid()) {
        const auto rect = extent(layout, previous);
        return {rect.left(), rect.bottom(), rect.width(), 0};
    }
    return {};
}

qreal TextBlocks::firstRowHeight(QAbstractTextDocumentLayout *layout, const QTextBlock &block) const
{
    // Force this block's layout before reading its visual rows. Wrapped
    // continuations already include the document's line-distance format.
    const auto rect = extent(layout, block);
    const auto *lines = block.layout();
    if (lines && lines->lineCount() > 1)
        return lines->lineAt(1).y() - lines->lineAt(0).y();
    return rect.height();
}

qreal TextBlocks::rowHeight() const
{
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout) return 0;
    const auto block = shown(text->begin(), true);
    return block.isValid() ? firstRowHeight(layout, block) : 0;
}

bool TextBlocks::folded(const QTextBlock &block) const
{
    return !block.isVisible() || rows_.contains(block.blockNumber() + 1);
}

QTextBlock TextBlocks::reachable(QTextBlock block, bool forward) const
{
    while (block.isValid() && folded(block)) block = forward ? block.next() : block.previous();
    return block;
}

QVariantList TextBlocks::between(qreal top, qreal bottom) const
{
    QVariantList blocks;
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout || bottom <= top) return blocks;
    const int first = layout->hitTest(QPointF(0, std::max<qreal>(0, top)), Qt::FuzzyHit);
    auto start = text->findBlock(std::max(0, first));
    // A fuzzy hit in the added gap can choose the following block even when
    // the preceding block's extent still intersects the requested area.
    const auto previous = shown(start.previous(), false);
    if (previous.isValid() && extent(layout, previous).bottom() > top) start = previous;
    for (auto block = start; block.isValid(); block = block.next()) {
        if (!block.isVisible()) continue;
        const auto rect = extent(layout, block);
        if (rect.top() >= bottom) break;
        if (rect.bottom() <= top) continue;
        blocks.append(QVariantMap{{QStringLiteral("number"), block.blockNumber() + 1},
            {QStringLiteral("position"), block.position()},
            {QStringLiteral("top"), rect.top()}, {QStringLiteral("height"), rect.height()},
            {QStringLiteral("rowHeight"), firstRowHeight(layout, block)}});
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
    auto rect = extent(layout, block);
    // A fuzzy hit can land on a neighbouring block at a boundary.
    for (auto previous = shown(block.previous(), false); y < rect.top() && previous.isValid();
         previous = shown(block.previous(), false)) {
        block = previous;
        rect = extent(layout, block);
    }
    for (auto next = shown(block.next(), true); y >= rect.bottom() && next.isValid(); next = shown(block.next(), true)) {
        block = next;
        rect = extent(layout, block);
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
    if (!text || (hiddenCount_ == 0 && rows_.isEmpty()) || position < 0 || position >= text->characterCount())
        return false;
    return folded(text->findBlock(position));
}

bool TextBlocks::hiddenWithin(int begin, int end) const
{
    const auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text || (hiddenCount_ == 0 && rows_.isEmpty()) || end <= begin) return false;
    for (auto block = text->findBlock(std::max(0, begin)); block.isValid() && block.position() < end;
         block = block.next()) {
        if (folded(block)) return true;
    }
    return false;
}

int TextBlocks::shownPosition(int position, bool forward, int from) const
{
    auto *text = document_ ? document_->textDocument() : nullptr;
    if (!text || !hiddenAt(position)) return position;
    const auto block = text->findBlock(position);
    const auto after = reachable(block, true);
    const auto before = reachable(block, false);
    const bool down = forward ? after.isValid() : !before.isValid();
    const auto target = down ? after : before;
    if (!target.isValid()) return position;
    const int edge = down ? target.position() : target.position() + target.length() - 1;
    const auto source = from >= 0 ? text->findBlock(from) : QTextBlock{};
    if (!source.isValid() || folded(source) || !source.layout()) return edge;
    const auto sourceLine = source.layout()->lineForTextPosition(from - source.position());
    // Laying out the document gives the target block its lines.
    (void)text->documentLayout()->blockBoundingRect(target);
    auto *layout = target.layout();
    if (!sourceLine.isValid() || !layout || layout->lineCount() == 0) return edge;
    const auto line = layout->lineAt(down ? 0 : layout->lineCount() - 1);
    return target.position() + line.xToCursor(sourceLine.cursorToX(from - source.position()));
}

QVariantList TextBlocks::whitespaceWithin(const QRectF &area, qreal stroke, qreal tabWidth) const
{
    QVariantList markers;
    auto *text = document_ ? document_->textDocument() : nullptr;
    auto *layout = text ? text->documentLayout() : nullptr;
    if (!layout || area.isEmpty()) return markers;
    const int first = layout->hitTest(QPointF(0, std::max<qreal>(0, area.top())), Qt::FuzzyHit);
    for (auto block = text->findBlock(std::max(0, first)); block.isValid(); block = block.next()) {
        if (!block.isVisible()) continue;
        const auto rect = layout->blockBoundingRect(block);
        if (rect.top() >= area.bottom()) break;
        if (rect.bottom() < area.top() || folded(block)) continue;
        appendWhitespace(markers, block, rect.topLeft(), area, stroke, tabWidth);
    }
    return markers;
}

void TextBlocks::keyPressed(int key)
{
    cursorKey_ = key;
    queueCursorCheck();
}

void TextBlocks::queueCursorCheck()
{
    if (cursorCheckQueued_) return;
    cursorCheckQueued_ = true;
    QMetaObject::invokeMethod(this, [this] {
        cursorCheckQueued_ = false;
        Q_EMIT cursorCheckDue();
    }, Qt::QueuedConnection);
}

int TextBlocks::settledPosition(int position, int anchor)
{
    const int key = std::exchange(cursorKey_, 0);
    int target = position;
    if (hiddenAt(position)) {
        const bool down = key == Qt::Key_Down || key == Qt::Key_PageDown;
        const bool up = key == Qt::Key_Up || key == Qt::Key_PageUp;
        const bool forward = down ? true : up ? false : key != 0 ? position > settledCursor_ : anchor >= position;
        target = shownPosition(position, forward, down || up ? settledCursor_ : -1);
    }
    settledCursor_ = target;
    return target;
}
}
