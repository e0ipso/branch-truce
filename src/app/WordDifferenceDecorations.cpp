// Word difference decorations highlight changed words without changing the result text.
#include "WordDifferenceDecorations.h"
#include "QtText.h"
#include "RevisionWorkers.h"
#include "core/FileWordDifferences.h"
#include "core/internal/ComputationInput.h"
#include "highlighting/PaneHighlighter.h"
#include "session/SessionStore.h"
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
using qt_text::documentText;
namespace {
const char *const kStyle = "wordDifferenceStyle";
// The share of the role colour the range background takes. The background
// roles are already this hue mixed into the base colour, so a higher share
// of it reads as a stronger tint of the same role.
constexpr float kTint = 0.35F;

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

WordDifferenceDecorations::WordDifferenceDecorations(const SessionStore &store, RevisionWorkers &workers,
                                                     QObject *parent)
    : QObject(parent), files_(store), workers_(workers)
{
    connect(&workers_, &RevisionWorkers::available, this, &WordDifferenceDecorations::refresh, Qt::QueuedConnection);
    connect(&files_, &SessionStore::fileChanged, this, &WordDifferenceDecorations::refresh);
    connect(&files_, &SessionStore::replaced, this, &WordDifferenceDecorations::refresh);
    connect(&files_, &SessionStore::aboutToReplace, this, [this] {
        ++request_;
        ranges_.clear();
        fileId_.clear();
        mapping_ = nullptr;
        captures_ = nullptr;
    });
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
    if (active_ || !file || !file->correspondence() || !file->snapshots()) return;
    const auto request = request_;
    const auto retained = detail::ComputationInput::retainedBytes(*file);
    if (retained > workers_.admission.maxBytes) {
        needsCompute_ = false;
        return;
    }
    active_ = workers_.submit(
        retained, this, true,
        [file] {
            return [input = detail::ComputationInput(*file)] {
                std::map<QString, PaneRanges> ranges;
                if (!input.editable) return ranges;
                const auto words = fileWordDifferences(
                    {*input.snapshots, *input.revision.correspondence, input.choices, input.revision.draft.decisions});
                for (const auto &[side, found] : words.sources) {
                    const auto &source = input.snapshots->source(side);
                    if (!source) continue;
                    const auto &text = source->worktree.utf8;
                    ranges[paneName(side)] = {QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())),
                                              utf16Ranges(text, found)};
                }
                const auto &text = input.revision.correspondence->text();
                ranges[QStringLiteral("result")] = {QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())),
                                                    utf16Ranges(text, words.result)};
                return ranges;
            };
        },
        [this, request](const auto &computed) {
            active_ = false;
            const auto *file = files_.file(targetFileId_.toStdString());
            if (request == request_ && file && targetFileId_ == fileId_ && file->current().id == revision_
                && file->correspondence() == mapping_ && file->snapshots() == captures_) {
                if (const auto *ranges = std::get_if<decltype(ranges_)>(&computed)) ranges_ = *ranges;
            }
            refresh();
        });
    if (active_) needsCompute_ = false;
}

QList<QTextLayout::FormatRange> WordDifferenceDecorations::formats(const Pane &pane, const PaneRanges *ranges) const
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
    const PaneRanges none;
    for (const auto &[start, length] : (ranges ? *ranges : none).ranges) {
        const auto block = pane.document->findBlock(start);
        const auto kind = lines.value(QString::number(block.blockNumber() + 1)).toString();
        // The same role the line background takes for its kind.
        const auto role = kind == QLatin1String("added") || kind == QLatin1String("accepted")
            ? QStringLiteral("positive")
            : kind == QLatin1String("conflict") || kind == QLatin1String("markers") ? QStringLiteral("negative")
            : kind == QLatin1String("manual")                                       ? QStringLiteral("neutral")
            : kind == QLatin1String("ignored")                                      ? QStringLiteral("disabled")
                                                                                    : QStringLiteral("active");
        QTextLayout::FormatRange range;
        range.start = start;
        range.length = length;
        range.format = format(role);
        result.push_back(range);
    }
    // An ignored side has no background fill; its text takes the disabled colour.
    if (const auto disabled = values.value(QStringLiteral("disabled")).value<QColor>(); disabled.isValid()) {
        QTextCharFormat tint;
        tint.setForeground(disabled);
        for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
            bool number = false;
            const auto block = pane.document->findBlockByNumber(it.key().toInt(&number) - 1);
            if (!number || it.value().toString() != QLatin1String("ignored") || !block.isValid() || block.length() <= 1)
                continue;
            QTextLayout::FormatRange range;
            range.start = block.position();
            range.length = block.length() - 1;
            range.format = tint;
            result.push_back(range);
        }
    }
    return result;
}

void WordDifferenceDecorations::refresh()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (refreshing_) return;
    QScopedValueRollback guard(refreshing_, true);
    const auto fileId = targetFileId_;
    const auto *file = files_.file(fileId.toStdString());
    if (!file || fileId != fileId_ || file->current().id != revision_ || file->correspondence() != mapping_
        || file->snapshots() != captures_) {
        ++request_;
        fileId_ = fileId;
        revision_ = file ? file->current().id : 0;
        mapping_ = file ? file->correspondence() : nullptr;
        captures_ = file ? file->snapshots() : nullptr;
        ranges_.clear();
        needsCompute_ = file && mapping_ && captures_;
    }
    if (needsCompute_) compute(fileId);
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
        highlighter->setDecorations(formats(pane, pane.matches ? &found->second : nullptr));
    }
    if (publishedPending_ != pending()) {
        publishedPending_ = pending();
        Q_EMIT pendingChanged();
    }
}
}
