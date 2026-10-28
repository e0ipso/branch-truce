#include "WordDifferenceDecorations.h"
#include "PaneHighlighter.h"
#include "SessionModel.h"
#include "core/WordDifferences.h"
#include <QColor>
#include <QJSValue>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QVariantMap>
#include <string_view>
#include <vector>

namespace branch_truce {
namespace {
const char *const kStyle = "wordDifferenceStyle";
// The share of the role colour the range background takes. The background
// roles are already this hue mixed into the base colour, so a higher share
// of it reads as a stronger tint of the same role.
constexpr float kTint = 0.35F;

QString documentText(const QTextDocument &document)
{
    return document.toRawText().replace(QChar(0x2029), QLatin1Char('\n'));
}

QVariantMap style(const QObject &editor)
{
    const auto value = editor.property(kStyle);
    if (value.metaType() == QMetaType::fromType<QJSValue>()) return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}

QString paneName(SourceSide side)
{
    switch (side) {
    case SourceSide::Earlier: return QStringLiteral("earlier");
    case SourceSide::Stage2: return QStringLiteral("stage2");
    case SourceSide::Stage3: break;
    }
    return QStringLiteral("stage3");
}

// UTF-8 byte ranges of text as UTF-16 [start, length) pairs. Ranges are
// ordered, disjoint and on character boundaries.
QList<std::pair<int, int>> utf16Ranges(std::string_view text, const std::vector<ByteRange> &ranges)
{
    QList<std::pair<int, int>> converted;
    converted.reserve(static_cast<qsizetype>(ranges.size()));
    std::size_t byte = 0;
    int unit = 0;
    const auto advance = [&](std::size_t to) {
        for (; byte < to && byte < text.size(); ++byte) {
            const auto c = static_cast<unsigned char>(text[byte]);
            if ((c & 0xC0U) != 0x80U) unit += c >= 0xF0U ? 2 : 1;
        }
        return unit;
    };
    for (const auto &range : ranges) {
        const int start = advance(range.begin);
        const int end = advance(range.end);
        if (end > start) converted.push_back({start, end - start});
    }
    return converted;
}
}

WordDifferenceDecorations::WordDifferenceDecorations(SessionModel &files, std::function<QString()> selectedFileId,
                                                     QObject *parent)
    : QObject(parent), files_(files), selectedFileId_(std::move(selectedFileId))
{
}

void WordDifferenceDecorations::attach(QObject *editor, QTextDocument *document)
{
    if (!editor || !document) return;
    for (const auto &pane : std::as_const(panes_))
        if (pane.editor == editor && pane.document == document) return;
    const auto name = style(*editor).value(QStringLiteral("pane")).toString();
    if (name.isEmpty()) return;
    Pane pane;
    pane.editor = editor;
    pane.document = document;
    pane.name = name;
    panes_.push_back(pane);
    const auto index = editor->metaObject()->indexOfProperty(kStyle);
    const auto refreshSlot = staticMetaObject.method(staticMetaObject.indexOfSlot("refresh()"));
    if (const auto property = editor->metaObject()->property(index); property.hasNotifySignal())
        connect(editor, property.notifySignal(), this, refreshSlot);
    connect(document, &QTextDocument::contentsChange, this, [this, document] {
        for (auto &pane : panes_)
            if (pane.document == document) pane.dirty = true;
    });
    // The result editor's text changes reach the session first, and its
    // adapter refreshes once the revision they make is current. A source
    // pane's text changes only when the view loads another file.
    if (name != QStringLiteral("result"))
        connect(document, &QTextDocument::contentsChanged, this, &WordDifferenceDecorations::refresh);
    refresh();
}

void WordDifferenceDecorations::compute(const QString &fileId)
{
    const auto *file = files_.file(fileId.toStdString());
    fileId_ = fileId;
    revision_ = file ? file->current().id : 0;
    // A file opens as a placeholder and keeps its id and revision once its
    // captures load, so the correspondence and captures identify the input.
    mapping_ = file ? file->correspondence() : nullptr;
    captures_ = file ? file->snapshots() : nullptr;
    ranges_.clear();
    if (!file) return;
    const auto words = fileWordDifferences(*file);
    if (const auto *snapshots = file->snapshots()) {
        for (const auto &[side, ranges] : words.sources) {
            const auto &source = side == SourceSide::Earlier ? snapshots->earlier
                : side == SourceSide::Stage2 ? snapshots->stage2 : snapshots->stage3;
            if (!source) continue;
            const auto &text = source->worktree.utf8;
            ranges_[paneName(side)] = {QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())),
                utf16Ranges(text, ranges)};
        }
    }
    if (const auto *map = file->correspondence(); map && !words.result.empty()) {
        const auto &text = map->text();
        ranges_[QStringLiteral("result")] = {QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())),
            utf16Ranges(text, words.result)};
    }
}

QList<QTextLayout::FormatRange> WordDifferenceDecorations::formats(const Pane &pane, const PaneRanges &ranges) const
{
    const auto values = style(*pane.editor);
    const auto lines = values.value(QStringLiteral("lines")).toMap();
    std::map<QString, QTextCharFormat> byRole;
    const auto format = [&](const QString &role) -> const QTextCharFormat & {
        auto found = byRole.find(role);
        if (found != byRole.end()) return found->second;
        const auto colour = values.value(role).value<QColor>();
        QTextCharFormat made;
        if (colour.isValid()) {
            auto tint = colour;
            tint.setAlphaF(colour.alphaF() * kTint);
            made.setBackground(tint);
            made.setUnderlineColor(colour);
        }
        made.setUnderlineStyle(QTextCharFormat::SingleUnderline);
        return byRole.emplace(role, made).first->second;
    };
    QList<QTextLayout::FormatRange> result;
    result.reserve(ranges.ranges.size());
    for (const auto &[start, length] : ranges.ranges) {
        const auto block = pane.document->findBlock(start);
        const auto kind = lines.value(QString::number(block.blockNumber() + 1)).toString();
        // The same role the line background takes for its kind.
        const auto role = kind == QLatin1String("added") || kind == QLatin1String("accepted") ? QStringLiteral("positive")
            : kind == QLatin1String("conflict") || kind == QLatin1String("markers") ? QStringLiteral("negative")
            : kind == QLatin1String("manual") ? QStringLiteral("neutral")
            : kind == QLatin1String("ignored") ? QStringLiteral("disabled")
            : QStringLiteral("active");
        QTextLayout::FormatRange range;
        range.start = start;
        range.length = length;
        range.format = format(role);
        result.push_back(range);
    }
    return result;
}

void WordDifferenceDecorations::refresh()
{
    if (refreshing_) return;
    QScopedValueRollback guard(refreshing_, true);
    const auto fileId = selectedFileId_();
    const auto *file = files_.file(fileId.toStdString());
    if (!file || fileId != fileId_ || file->current().id != revision_ || file->correspondence() != mapping_
        || file->snapshots() != captures_)
        compute(fileId);
    panes_.removeIf([](const Pane &pane) { return !pane.editor || !pane.document; });
    for (auto &pane : panes_) {
        auto *highlighter = pane.document->findChild<PaneHighlighter *>(QString(), Qt::FindDirectChildrenOnly);
        if (!highlighter) continue;
        const auto found = ranges_.find(pane.name);
        const auto input = pane.name == QLatin1String("result") ? static_cast<const void *>(mapping_) : captures_;
        const auto checkedFor = fileId_ + QLatin1Char('@') + QString::number(reinterpret_cast<quintptr>(input));
        if (found == ranges_.end()) {
            pane.matches = false;
            pane.checkedFor.clear();
        } else if (pane.dirty || pane.checkedFor != checkedFor) {
            pane.matches = documentText(*pane.document) == found->second.text;
            pane.dirty = false;
            pane.checkedFor = checkedFor;
        }
        highlighter->setDecorations(pane.matches ? formats(pane, found->second) : QList<QTextLayout::FormatRange>{});
    }
}
}
