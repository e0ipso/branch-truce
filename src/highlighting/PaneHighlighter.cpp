// Pane highlighting combines syntax colours and word decorations without editing document text.
#include "highlighting/PaneHighlighter.h"
#include <QTextBlock>
#include <QTextCursor>
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
    decorations_ = std::move(sorted);
    reach_.clear();
    reach_.reserve(decorations_.size());
    for (const auto &range : std::as_const(decorations_))
        reach_.push_back(std::max(reach_.isEmpty() ? 0 : reach_.back(), range.start + range.length));
    auto *target = document();
    if (!target) return;
    if (appliedDocument_ != target) {
        appliedDecorations_.clear();
        appliedDocument_ = target;
    }
    appliedDecorations_.removeIf([target](const AppliedDecorations &entry) {
        return !entry.block.isValid() || entry.block.document() != target;
    });
    // Old overlays follow their live blocks across edits. Include them to
    // clear removed ranges, then add the blocks wanted by the new overlays.
    std::set<int> candidates;
    for (const auto &entry : std::as_const(appliedDecorations_))
        candidates.insert(entry.block.blockNumber());
    for (const auto &range : std::as_const(decorations_)) {
        const auto first = target->findBlock(range.start);
        if (!first.isValid()) continue;
        const auto last = target->findBlock(range.start + std::max(range.length, 1) - 1);
        const int lastNumber = last.isValid() ? last.blockNumber() : target->blockCount() - 1;
        for (int number = first.blockNumber(); number <= lastNumber; ++number) candidates.insert(number);
    }
    std::set<int> blocks;
    for (const int number : candidates) {
        const auto block = target->findBlockByNumber(number);
        const auto wanted = blockDecorations(block.position(), block.position() + block.length() - 1);
        const auto found = std::find_if(appliedDecorations_.cbegin(), appliedDecorations_.cend(),
            [&block](const AppliedDecorations &entry) { return entry.block == block; });
        if (found == appliedDecorations_.cend()) {
            if (wanted.isEmpty()) continue;
        } else if (found->revision == block.revision() && found->text == block.text() && found->ranges == wanted) {
            continue;
        }
        blocks.insert(number);
    }
    if (blocks.empty()) return;
    if (static_cast<qsizetype>(blocks.size()) * 2 > target->blockCount()) {
        rehighlight();
        return;
    }
    // Rehighlighting each block separately otherwise publishes a document
    // change and relayout for every range. Publish the decoration pass once.
    const bool modified = target->isModified();
    QTextCursor transaction(target);
    transaction.beginEditBlock();
    for (const int number : blocks) rehighlightBlock(target->findBlockByNumber(number));
    transaction.endEditBlock();
    target->setModified(modified);
}

void PaneHighlighter::highlightBlock(const QString &text)
{
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
    if (definition().isValid()) SyntaxHighlighter::highlightBlock(text);
#endif
    const int begin = currentBlock().position();
    const auto ranges = blockDecorations(begin, begin + static_cast<int>(text.size()));
    decorate(ranges);
    rememberDecorations(currentBlock(), text, ranges);
}

QList<QTextLayout::FormatRange> PaneHighlighter::blockDecorations(int begin, int end) const
{
    QList<QTextLayout::FormatRange> result;
    const auto append = [&](const QTextLayout::FormatRange &range) {
        const int first = std::max(range.start, begin), last = std::min(range.start + range.length, end);
        if (last <= first) return;
        auto local = range;
        local.start = first - begin;
        local.length = last - first;
        result.append(local);
    };
    auto from = std::upper_bound(reach_.cbegin(), reach_.cend(), begin) - reach_.cbegin();
    for (; from < decorations_.size() && decorations_[from].start < end; ++from)
        append(decorations_[from]);
    // Keep search's final overlay after all word/syntax formatting.
    auto match = std::lower_bound(searchDecorations_.cbegin(), searchDecorations_.cend(), begin,
        [](const auto &range, int position) { return range.start + range.length <= position; });
    for (; match != searchDecorations_.cend() && match->start < end; ++match) append(*match);
    return result;
}

void PaneHighlighter::rememberDecorations(const QTextBlock &block, const QString &text,
                                         const QList<QTextLayout::FormatRange> &ranges)
{
    if (appliedDocument_ != document()) {
        appliedDecorations_.clear();
        appliedDocument_ = document();
    }
    const auto found = std::find_if(appliedDecorations_.begin(), appliedDecorations_.end(),
        [&block](const AppliedDecorations &entry) { return entry.block == block; });
    if (ranges.isEmpty()) {
        if (found != appliedDecorations_.end()) appliedDecorations_.erase(found);
    } else {
        const AppliedDecorations applied{block, text, block.revision(), ranges};
        if (found == appliedDecorations_.end()) appliedDecorations_.append(applied);
        else *found = applied;
    }
}

// Merges each block-local decoration into the formats already set, one run
// of equal formats at a time, so syntax colours stay under it.
void PaneHighlighter::decorate(const QList<QTextLayout::FormatRange> &ranges)
{
    const auto apply = [&](const QTextLayout::FormatRange &range) {
        int at = range.start;
        const int stop = range.start + range.length;
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
    for (const auto &range : ranges) apply(range);
}
}
