#include "highlighting/PaneHighlighter.h"
#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>
#include <set>
#include <utility>

namespace branch_truce {
PaneHighlighter::PaneHighlighter(QTextDocument *document)
    : PaneHighlighterBase(static_cast<QObject *>(document))
{
}

void PaneHighlighter::attach()
{
    auto *target = qobject_cast<QTextDocument *>(parent());
    if (document() != target) setDocument(target);
}

#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
void PaneHighlighter::highlight(const KSyntaxHighlighting::Definition &definition,
    const KSyntaxHighlighting::Theme &theme)
{
    // setDefinition rehighlights an attached document when the definition
    // changes; setTheme never does.
    const bool recolour = theme.name() != this->theme().name();
    const bool redefine = definition != this->definition();
    if (recolour) setTheme(theme);
    setDefinition(definition);
    if (document() != parent()) attach();
    else if (recolour && !redefine && definition.isValid()) rehighlight();
}
#endif

void PaneHighlighter::setDecorations(const QList<QTextLayout::FormatRange> &ranges)
{
    if (wordDecorations_ == ranges) return;
    wordDecorations_ = ranges;
    updateDecorations();
}

void PaneHighlighter::setSearchDecorations(const QList<QTextLayout::FormatRange> &ranges)
{
    if (searchDecorations_ == ranges) return;
    searchDecorations_ = ranges;
    updateDecorations();
}

void PaneHighlighter::updateDecorations()
{
    auto sorted = wordDecorations_;
    sorted.append(searchDecorations_);
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
    if (sorted == decorations_) return;
    const auto previous = std::exchange(decorations_, std::move(sorted));
    reach_.clear();
    reach_.reserve(decorations_.size());
    for (const auto &range : std::as_const(decorations_))
        reach_.push_back(std::max(reach_.isEmpty() ? 0 : reach_.back(), range.start + range.length));
    auto *target = document();
    if (!target) return;
    // A block formatted with the previous ranges after an edit lies under them
    // in current positions, so these blocks cover it.
    std::set<int> blocks;
    const QList<QTextLayout::FormatRange> *lists[] = {&previous, &decorations_};
    for (const auto *list : lists) {
        for (const auto &range : *list) {
            const auto first = target->findBlock(range.start);
            if (!first.isValid()) continue;
            const auto last = target->findBlock(range.start + std::max(range.length, 1) - 1);
            const int lastNumber = last.isValid() ? last.blockNumber() : target->blockCount() - 1;
            for (int number = first.blockNumber(); number <= lastNumber; ++number) blocks.insert(number);
        }
    }
    if (static_cast<qsizetype>(blocks.size()) * 2 > target->blockCount()) {
        rehighlight();
        return;
    }
    for (const int number : blocks) rehighlightBlock(target->findBlockByNumber(number));
}

void PaneHighlighter::highlightBlock(const QString &text)
{
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
    if (definition().isValid()) SyntaxHighlighter::highlightBlock(text);
#endif
    const int begin = currentBlock().position();
    decorate(begin, begin + static_cast<int>(text.size()));
}

// Merges each decoration into the formats already set on [begin, end), one
// run of equal formats at a time, so syntax colours stay under it.
void PaneHighlighter::decorate(int begin, int end)
{
    const auto apply = [&](const QTextLayout::FormatRange &range) {
        int at = std::max(range.start, begin) - begin;
        const int stop = std::min(range.start + range.length, end) - begin;
        while (at < stop) {
            const auto under = format(at);
            int run = at + 1;
            while (run < stop && format(run) == under) ++run;
            auto merged = under;
            merged.merge(range.format);
            setFormat(at, run - at, merged);
            at = run;
        }
    };
    auto from = std::upper_bound(reach_.cbegin(), reach_.cend(), begin) - reach_.cbegin();
    for (; from < decorations_.size() && decorations_[from].start < end; ++from)
        apply(decorations_[from]);
    // Search backgrounds win over a word-difference background on the same text.
    auto match = std::lower_bound(searchDecorations_.cbegin(), searchDecorations_.cend(), begin,
        [](const auto &range, int position) { return range.start + range.length <= position; });
    for (; match != searchDecorations_.cend() && match->start < end; ++match) apply(*match);
}
}
