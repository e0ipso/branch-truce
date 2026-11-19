// Correspondence: the algorithm contract, exact line passages, anchors and the map's input checks.
#include "TestHarness.h"
#include "core/Correspondence.h"
#include "core/ExactLineCorrespondence.h"
#include "core/ResourceLimits.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace branch_truce;

namespace {
// The least an implementation may do: the whole of both texts as one
// passage, Equal when they match and otherwise Ambiguous.
class WholeTextCorrespondence final : public CorrespondenceAlgorithm {
public:
    [[nodiscard]] std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        ++calls;
        return {
            {{0, source.size()}, {0, result.size()}, source == result ? PassageKind::Equal : PassageKind::Ambiguous}};
    }
    [[nodiscard]] std::string_view identity() const noexcept override { return "whole-text"; }
    mutable std::atomic<int> calls{0};
};

// The algorithm contract of Correspondence.h, for any implementation.
void keepsTheContract(const CorrespondenceAlgorithm &algorithm)
{
    const std::array<std::pair<std::string_view, std::string_view>, 7> cases{{
        {"", ""},
        {"same\n", "same\n"},
        {"head\nold\nend\n", "head\nnew\nend\n"},
        {"head\nend\n", "head\ninserted\nend\n"},
        {"head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n"},
        {"no final newline", "no final newline\n"},
        {"\xc3\xa9t\xc3\xa9\n", "\xf0\x9f\x98\x80\n"},
    }};
    for (const auto &[source, result] : cases) {
        const auto passages = algorithm.compare(source, result);
        std::size_t sourceEnd = 0;
        std::size_t resultEnd = 0;
        for (const auto &passage : passages) {
            CHECK(passage.source.begin == sourceEnd && passage.result.begin == resultEnd);
            CHECK(validByteRange(source, passage.source) && validByteRange(result, passage.result));
            if (passage.kind == PassageKind::Equal)
                CHECK(slice(source, passage.source) == slice(result, passage.result));
            sourceEnd = passage.source.end;
            resultEnd = passage.result.end;
        }
        CHECK(sourceEnd == source.size() && resultEnd == result.size());
        CHECK(algorithm.compare(source, result) == passages);
    }
    // Two equally good places for one repeated line are never a guess.
    for (const auto &passage : algorithm.compare("repeat\n", "repeat\nrepeat\n"))
        CHECK(passage.kind != PassageKind::Equal || passage.result.end - passage.result.begin != 7);
}

void implementationsKeepTheAlgorithmContract()
{
    keepsTheContract(ExactLineCorrespondence{});
    const WholeTextCorrespondence whole;
    const ExactLineCorrespondence exact;
    auto worker = std::async(std::launch::async, [&] {
        keepsTheContract(whole);
        keepsTheContract(exact);
    });
    keepsTheContract(whole);
    keepsTheContract(exact);
    worker.get();
    CHECK(whole.calls == 30);
}

const char *name(PassageKind kind)
{
    switch (kind) {
    case PassageKind::Equal: return "equal";
    case PassageKind::Changed: return "changed";
    case PassageKind::SourceOnly: return "source only";
    case PassageKind::ResultOnly: return "result only";
    case PassageKind::Ambiguous: break;
    }
    return "ambiguous";
}

std::string show(const std::vector<Passage> &passages)
{
    std::string text;
    for (const auto &passage : passages) {
        text += name(passage.kind);
        text += ' ';
        text += std::to_string(passage.source.begin);
        text += '-';
        text += std::to_string(passage.source.end);
        text += ' ';
        text += std::to_string(passage.result.begin);
        text += '-';
        text += std::to_string(passage.result.end);
        text += '\n';
    }
    return text;
}

// ExactLineCorrespondence gives exactly `expected`, every time, as a partition
// of both texts whose equal passages cover identical bytes.
void expectPassages(std::string_view source, std::string_view result, const std::vector<Passage> &expected)
{
    const ExactLineCorrespondence algorithm;
    const auto actual = algorithm.compare(source, result);
    if (actual != expected) {
        throw std::runtime_error("passages differ\nactual\n" + show(actual) + "expected\n" + show(expected));
    }
    CHECK(actual == expected);
    for (int repeat = 0; repeat < 4; ++repeat) CHECK(algorithm.compare(source, result) == actual);
    std::size_t sourceEnd = 0;
    std::size_t resultEnd = 0;
    for (const auto &passage : actual) {
        CHECK(passage.source.begin == sourceEnd && passage.result.begin == resultEnd);
        CHECK(validByteRange(source, passage.source) && validByteRange(result, passage.result));
        if (passage.kind == PassageKind::Equal) CHECK(slice(source, passage.source) == slice(result, passage.result));
        sourceEnd = passage.source.end;
        resultEnd = passage.result.end;
    }
    CHECK(sourceEnd == source.size() && resultEnd == result.size());
}

void partitionsAreDeterministicForEachKindOfPassage()
{
    expectPassages("repeat\nrepeat\n", "repeat\nrepeat\n", {{{0, 14}, {0, 14}, PassageKind::Equal}});
    expectPassages("head\nold\nend\n", "head\nnew\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 9}, {5, 9}, PassageKind::Changed},
                    {{9, 13}, {9, 13}, PassageKind::Equal}});
    expectPassages("head\nend\n", "head\ninserted\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 5}, {5, 14}, PassageKind::ResultOnly},
                    {{5, 9}, {14, 18}, PassageKind::Equal}});
    expectPassages("head\ndeleted\nend\n", "head\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 13}, {5, 5}, PassageKind::SourceOnly},
                    {{13, 17}, {5, 9}, PassageKind::Equal}});
    expectPassages("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 19}, {5, 12}, PassageKind::Ambiguous},
                    {{19, 23}, {12, 16}, PassageKind::Equal}});
    expectPassages("head\none\ntwo\nend\n", "head\ntwo\none\nend\n",
                   {{{0, 5}, {0, 5}, PassageKind::Equal},
                    {{5, 13}, {5, 13}, PassageKind::Ambiguous},
                    {{13, 17}, {13, 17}, PassageKind::Equal}});
}

void aLongUniqueTextAlignsAsOneEqualPassage()
{
    std::string source;
    for (int line = 0; line < 3500; ++line) {
        source += "unique line ";
        source += std::to_string(line);
        source += '\n';
    }
    const auto result = "manual prefix\n" + source + "manual suffix\n";
    const auto passages = ExactLineCorrespondence{}.compare(source, result);
    CHECK(passages
          == std::vector<Passage>{
              {{0, 0}, {0, 14}, PassageKind::ResultOnly},
              {{0, source.size()}, {14, 14 + source.size()}, PassageKind::Equal},
              {{source.size(), source.size()}, {14 + source.size(), result.size()}, PassageKind::ResultOnly}});
}

std::string uniqueLines(std::string_view prefix, std::size_t count)
{
    std::string text;
    for (std::size_t line = 0; line < count; ++line) {
        text += prefix;
        text += std::to_string(line);
        text += '\n';
    }
    return text;
}

void textsOverTheCellBudgetAreOneAmbiguousPassage()
{
    // No line of 8193 against 8193 is shared, so every line is changed: far
    // over the changed-line cap, and the banded fallback's 8193 squared cells
    // are just over kMaxAlignmentCells. cost() only estimates admission: the
    // changed lines times the difference of their counts, at least one.
    const auto source = uniqueLines("unique line ", 8193);
    const auto result = uniqueLines("other line ", 8193);
    CHECK(8193UL * 8193UL > kMaxAlignmentCells && 8192UL * 8192UL <= kMaxAlignmentCells);
    CHECK_EQ(ExactLineCorrespondence{}.cost(source, result), 8193UL + 8193UL);
    CHECK(ExactLineCorrespondence{}.compare(source, result)
          == std::vector<Passage>{{{0, source.size()}, {0, result.size()}, PassageKind::Ambiguous}});
    // Identical texts need no alignment, whatever their size.
    CHECK(ExactLineCorrespondence{}.cost(source, source) == 0);
    CHECK(ExactLineCorrespondence{}.compare(source, source)
          == std::vector<Passage>{{{0, source.size()}, {0, source.size()}, PassageKind::Equal}});
    // Over the index limit nothing is aligned.
    const auto longest = std::string(kMaxIndexedLines + 1, '\n');
    CHECK(ExactLineCorrespondence{}.cost(longest, "x\n") == std::numeric_limits<std::size_t>::max());
}

void sharedEdgesAlignLongTextsWithFewChanges()
{
    // A lockfile-sized pair that differs in one line: every optimal alignment
    // stays within one line of the diagonal, so it costs a narrow band.
    const auto head = uniqueLines("head line ", 12000);
    const auto tail = uniqueLines("tail line ", 12000);
    const auto source = head + "ours\n" + tail;
    const auto result = head + "theirs\n" + tail;
    CHECK(lineCount(source) * lineCount(result) > kMaxAlignmentCells);
    CHECK(ExactLineCorrespondence{}.cost(source, result) <= kMaxAlignmentCells);
    expectPassages(source, result,
                   {{{0, head.size()}, {0, head.size()}, PassageKind::Equal},
                    {{head.size(), head.size() + 5}, {head.size(), head.size() + 7}, PassageKind::Changed},
                    {{head.size() + 5, source.size()}, {head.size() + 7, result.size()}, PassageKind::Equal}});
    // An inserted block between the shared edges aligns the same way.
    const auto inserted = head + uniqueLines("inserted line ", 50) + tail;
    expectPassages(head + tail, inserted,
                   {{{0, head.size()}, {0, head.size()}, PassageKind::Equal},
                    {{head.size(), head.size()}, {head.size(), inserted.size() - tail.size()}, PassageKind::ResultOnly},
                    {{head.size(), head.size() + tail.size()},
                     {inserted.size() - tail.size(), inserted.size()},
                     PassageKind::Equal}});
}

void farApartChangesInALongTextAlignAroundEachChange()
{
    // A lockfile-sized pair that differs in line 7 and line 12,001. The
    // shared edges leave almost every line between the two changes, so only
    // the changed-line count keeps this alignment small.
    const auto lines = [](std::size_t from, std::size_t to) {
        std::string text;
        for (auto line = from; line < to; ++line) text += "line " + std::to_string(line) + '\n';
        return text;
    };
    const auto head = lines(0, 6);
    const auto middle = lines(7, 12000);
    const auto tail = lines(12001, 24000);
    const auto source = head + "line 6\n" + middle + "line 12000\n" + tail;
    const auto result = head + "changed line 6\n" + middle + "changed line 12000\n" + tail;
    CHECK(lineCount(source) == 24000 && lineCount(result) == 24000);
    CHECK(ExactLineCorrespondence{}.cost(source, result) <= kMaxAlignmentCells);
    const auto second = head.size() + 7 + middle.size();
    expectPassages(source, result,
                   {{{0, head.size()}, {0, head.size()}, PassageKind::Equal},
                    {{head.size(), head.size() + 7}, {head.size(), head.size() + 15}, PassageKind::Changed},
                    {{head.size() + 7, second}, {head.size() + 15, second + 8}, PassageKind::Equal},
                    {{second, second + 11}, {second + 8, second + 27}, PassageKind::Changed},
                    {{second + 11, source.size()}, {second + 27, result.size()}, PassageKind::Equal}});
    // Version 3 made this pair one Ambiguous passage, so the passages a
    // journal recorded can differ and the name changes.
    CHECK_EQ(ExactLineCorrespondence{}.identity(), std::string_view{"exact-line/4"});
}

// Line pairs every optimal alignment of `source` and `result` matches, found
// without Hirschberg or a band. A match with rank k (k matches before it) is
// in every optimal alignment exactly when no other match can take rank k in
// an optimal alignment.
std::set<std::pair<std::size_t, std::size_t>> sharedMatches(const std::vector<std::string> &source,
                                                            const std::vector<std::string> &result)
{
    const auto n = source.size();
    const auto m = result.size();
    std::vector<std::vector<std::size_t>> before(n + 1, std::vector<std::size_t>(m + 1));
    std::vector<std::vector<std::size_t>> after(n + 2, std::vector<std::size_t>(m + 2));
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j)
            before[i][j] = source[i - 1] == result[j - 1] ? before[i - 1][j - 1] + 1
                                                          : std::max(before[i - 1][j], before[i][j - 1]);
    for (auto i = n; i-- > 0;)
        for (auto j = m; j-- > 0;)
            after[i][j] = source[i] == result[j] ? after[i + 1][j + 1] + 1 : std::max(after[i + 1][j], after[i][j + 1]);
    std::map<std::size_t, std::vector<std::pair<std::size_t, std::size_t>>> byRank;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < m; ++j)
            if (source[i] == result[j] && before[i][j] + 1 + after[i + 1][j + 1] == before[n][m])
                byRank[before[i][j]].emplace_back(i, j);
    std::set<std::pair<std::size_t, std::size_t>> shared;
    for (const auto &[rank, candidates] : byRank)
        if (candidates.size() == 1) shared.insert(candidates.front());
    return shared;
}

std::set<std::pair<std::size_t, std::size_t>> equalLinePairs(std::string_view source, std::string_view result,
                                                             const std::vector<Passage> &passages)
{
    const auto sourceStarts = lineStarts(source);
    const auto resultStarts = lineStarts(result);
    const auto index = [](const std::vector<std::size_t> &starts, std::size_t offset) {
        return static_cast<std::size_t>(std::lower_bound(starts.begin(), starts.end(), offset) - starts.begin());
    };
    std::set<std::pair<std::size_t, std::size_t>> pairs;
    for (const auto &passage : passages) {
        if (passage.kind != PassageKind::Equal) continue;
        auto i = index(sourceStarts, passage.source.begin);
        auto j = index(resultStarts, passage.result.begin);
        for (; i < index(sourceStarts, passage.source.end); ++i, ++j) pairs.emplace(i, j);
    }
    return pairs;
}

// Every text of up to `most` lines over "a" and "b", in a fixed order.
std::vector<std::vector<std::string>> allTexts(std::size_t most)
{
    std::vector<std::vector<std::string>> texts{{}};
    for (std::size_t from = 0; from < texts.size(); ++from)
        if (texts[from].size() < most)
            for (const auto *line : {"a\n", "b\n"}) {
                auto longer = texts[from];
                longer.emplace_back(line);
                texts.push_back(std::move(longer));
            }
    return texts;
}

std::string joined(const std::vector<std::string> &lines)
{
    std::string text;
    for (const auto &line : lines) text += line;
    return text;
}

void equalPassagesAreTheMatchesEveryOptimalAlignmentShares()
{
    // Two distinct lines make repeats, ties and ambiguity that reaches into
    // the shared edges common, which is where a band could go wrong. Every
    // pair of short middles between every short shared start and end.
    const auto edges = allTexts(2);
    const auto middles = allTexts(4);
    for (const auto &prefix : edges)
        for (const auto &suffix : edges)
            for (const auto &sourceMiddle : middles)
                for (const auto &resultMiddle : middles) {
                    auto source = prefix;
                    auto result = prefix;
                    source.insert(source.end(), sourceMiddle.begin(), sourceMiddle.end());
                    result.insert(result.end(), resultMiddle.begin(), resultMiddle.end());
                    source.insert(source.end(), suffix.begin(), suffix.end());
                    result.insert(result.end(), suffix.begin(), suffix.end());
                    const auto sourceText = joined(source);
                    const auto resultText = joined(result);
                    const auto passages = ExactLineCorrespondence{}.compare(sourceText, resultText);
                    if (equalLinePairs(sourceText, resultText, passages) != sharedMatches(source, result)) {
                        std::string message = "equal passages differ from the shared matches\nsource\n";
                        message += sourceText;
                        message += "result\n";
                        message += resultText;
                        message += "actual\n";
                        message += show(passages);
                        throw std::runtime_error(message);
                    }
                    CHECK(ExactLineCorrespondence{}.cost(sourceText, resultText) <= source.size() * result.size());
                }
}

std::shared_ptr<const FileSnapshots> sources(const std::string &text, const std::string &result)
{
    FileSnapshots captured;
    captured.path = "contract.txt";
    captured.provenance.sessionId = "contract";
    captured.stage2 = SourceSnapshot{"object", "100644", text, {text, text, {}}, "commit", "Source"};
    captured.launchResult.text = {result, result, {}};
    return std::make_shared<const FileSnapshots>(std::move(captured));
}

std::shared_ptr<const FileSnapshots> sources(const std::string &text) { return sources(text, text); }

bool rejected(const std::function<void()> &build)
{
    try {
        build();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

CorrespondenceMap mapped(const std::string &source, const std::string &result)
{
    return {1, sources(source, result), result, std::make_shared<const ExactLineCorrespondence>()};
}

void immediateAndDeferredEditsCheckJoinedLinesBeforeCopying()
{
    const auto map = mapped("short\n", "short\n");
    const ResultEdit replacement{{0, 6}, std::string(kMaxLineBytes + 1, 'x')};
    for (const auto alignment : {Realignment::Now, Realignment::Deferred}) {
        CHECK(rejected([&] { (void)CorrespondenceMap(map).edited(2, replacement, true, {}, alignment); }));
        auto full = mapped(std::string(kMaxLineBytes, 'x'), std::string(kMaxLineBytes, 'x'));
        CHECK(rejected([&] { (void)CorrespondenceMap(full).edited(2, {{0, 0}, "x"}, true, {}, alignment); }));
        const auto fit = CorrespondenceMap(full).edited(2, {{0, 1}, "y"}, true, {}, alignment);
        CHECK(fit.text().size() == kMaxLineBytes && fit.text().front() == 'y');
        CHECK(fit.realigned().text() == fit.text());
    }
    const std::string half(kMaxLineBytes / 2, 'x');
    const auto joined = mapped(half + "\n" + half, half + "\n" + half);
    CHECK(rejected([&] { (void)CorrespondenceMap(joined).edited(2, {{half.size(), half.size() + 1}, "x"}); }));
}

// The anchor bound to `id`. A missing one fails the case instead of crashing it.
ResultAnchor anchorOf(const CorrespondenceMap &map, const std::string &id)
{
    const auto *anchor = map.anchor(id);
    CHECK(anchor != nullptr);
    return *anchor;
}

ResultAnchor bound(CorrespondenceMap &map, const SourceAnchor &source)
{
    const auto id = source.contributionId;
    CHECK(map.bind(source));
    return anchorOf(map, id);
}

// The text after a choice replaces the anchor's range.
CorrespondenceMap chosen(const CorrespondenceMap &map, const std::string &id, const std::string &replacement)
{
    const auto *anchor = map.anchor(id);
    if (anchor == nullptr || !anchor->result) throw std::runtime_error("the anchor has no result range");
    return CorrespondenceMap(map).edited(map.revision() + 1, {*anchor->result, replacement}, false, id);
}

void aDeferredEditKeepsThePassagesAroundTheEditedLines()
{
    const std::string text = "a\nb\nc\nd\n";
    auto map = mapped(text, text);
    const auto anchor = bound(map, {"c", SourceSide::Stage2, {4, 6}});
    const ResultEdit edit{{4, 4}, "x"};
    const auto deferred = CorrespondenceMap(map).edited(2, edit, true, {}, Realignment::Deferred);
    const auto now = CorrespondenceMap(map).edited(2, edit);
    CHECK(!deferred.aligned() && now.aligned() && map.aligned());
    CHECK(deferred.text() == "a\nb\nxc\nd\n" && deferred.sameAnchors(now));
    CHECK(*deferred.passages(SourceSide::Stage2)
          == std::vector<Passage>({{{0, 4}, {0, 4}, PassageKind::Equal},
                                   {{4, 6}, {4, 7}, PassageKind::Ambiguous},
                                   {{6, 8}, {7, 9}, PassageKind::Equal}}));
    const auto realigned = deferred.realigned();
    CHECK(realigned.aligned() && realigned.sameAnchors(now));
    CHECK(*realigned.passages(SourceSide::Stage2) == *now.passages(SourceSide::Stage2));
    CHECK(anchorOf(realigned, "c") == anchorOf(now, "c") && anchor.result == ByteRange{4, 6});
    // Every edit leaves a partition of both texts, at either end too.
    for (const ResultEdit &other : {ResultEdit{{0, 0}, "start\n"}, ResultEdit{{8, 8}, "end"}, ResultEdit{{1, 7}, ""},
                                    ResultEdit{{0, 8}, "all new\n"}}) {
        const auto provisional = CorrespondenceMap(map).edited(2, other, true, {}, Realignment::Deferred);
        std::size_t source = 0;
        std::size_t result = 0;
        for (const auto &passage : *provisional.passages(SourceSide::Stage2)) {
            CHECK(passage.source.begin == source && passage.result.begin == result);
            if (passage.kind == PassageKind::Equal)
                CHECK(slice(text, passage.source) == slice(provisional.text(), passage.result));
            source = passage.source.end;
            result = passage.result.end;
        }
        CHECK(source == text.size() && result == provisional.text().size());
        CHECK(*provisional.realigned().passages(SourceSide::Stage2)
              == *CorrespondenceMap(map).edited(2, other).passages(SourceSide::Stage2));
    }
}

void repeatedAndReorderedPassagesLeaveAnchorsAmbiguous()
{
    for (const auto &[source, result] : {std::pair{"head\nrepeat\nrepeat\nend\n", "head\nrepeat\nend\n"},
                                         std::pair{"head\none\ntwo\nend\n", "head\ntwo\none\nend\n"}}) {
        auto map = mapped(source, result);
        const auto anchor = bound(map, {"choice", SourceSide::Stage2, {5, 12}});
        CHECK(anchor.state == AnchorState::Ambiguous && !anchor.result);
    }
}

void equalRepeatedPassagesKeepTheSelectedOccurrence()
{
    auto map = mapped("repeat\nmiddle\nrepeat\n", "manual prefix\nrepeat\nmiddle\nrepeat\n");
    const auto anchor = bound(map, {"choice", SourceSide::Stage2, {14, 21}});
    CHECK(anchor.state == AnchorState::Mapped && anchor.result == ByteRange{28, 35});
    CHECK(chosen(map, "choice", "selected\n").text() == "manual prefix\nrepeat\nmiddle\nselected\n");
}

void anchorsBindOnlyOnCharacterBoundaries()
{
    const std::string text = "start\n\xc3\xa9\xf0\x9f\x98\x80\nend\n";
    auto map = mapped(text, text);
    CHECK(!map.bind({"inside-codepoint", SourceSide::Stage2, {7, 13}}));
    CHECK(!map.bind({"", SourceSide::Stage2, {6, 13}}) && !map.bind({"other", SourceSide::Earlier, {0, 0}}));
    CHECK(bound(map, {"choice", SourceSide::Stage2, {6, 13}}).result == ByteRange{6, 13});
    CHECK(!map.bind({"choice", SourceSide::Stage2, {0, 6}}));
    CHECK(rejected([&] { (void)CorrespondenceMap(map).edited(2, {{7, 8}, "x"}); }));
    CHECK(rejected([&] { (void)CorrespondenceMap(map).edited(2, {{6, 13}, "\x80"}); }));
    CHECK(chosen(map, "choice", "valid\n").text() == "start\nvalid\nend\n");
}

void sourceOnlyAnchorsUseTheDeletionSite()
{
    auto map = mapped("head\nremoved\ntail\n", "manual prefix\nhead\ntail\nmanual suffix\n");
    const auto deleted = bound(map, {"choice", SourceSide::Stage2, {5, 13}});
    CHECK(deleted.state == AnchorState::Mapped && deleted.result == ByteRange{19, 19} && !deleted.manualOverlap);
    CHECK(chosen(map, "choice", "restored\n").text() == "manual prefix\nhead\nrestored\ntail\nmanual suffix\n");
}

void anchorsFollowManualInsertionsAndDeletions()
{
    auto map = mapped("head\nleft\nright\nend\n", "head\nleft\nright\nend\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {5, 10}});
    const std::string prefix = "manual \xc3\xa9\n";
    const std::string suffix = "unrelated manual tail\n";
    map = CorrespondenceMap(map).edited(2, {{0, 0}, prefix});
    map = CorrespondenceMap(map).edited(3, {{prefix.size(), prefix.size() + 5}, {}});
    map = CorrespondenceMap(map).edited(4, {{map.text().size(), map.text().size()}, suffix});
    const auto &current = anchorOf(map, "choice");
    CHECK(current.revision == 4 && current.result == ByteRange{prefix.size(), prefix.size() + 5});
    CHECK(!current.manualOverlap && !current.handEdited);
    CHECK(chosen(map, "choice", "accepted\n").text() == prefix + "accepted\nright\nend\n" + suffix);
}

void launchTextThatDiffersFromItsSourceNeedsConfirmation()
{
    auto map = mapped("head\nsource\nend\n", "head\nmanual\nend\n");
    const auto anchor = bound(map, {"choice", SourceSide::Stage2, {5, 12}});
    CHECK(anchor.manualOverlap && !anchor.handEdited && anchor.result == ByteRange{5, 12});
    const auto typed = CorrespondenceMap(map).edited(2, {{6, 7}, "X"});
    CHECK(anchorOf(typed, "choice").manualOverlap && anchorOf(typed, "choice").handEdited);
    CHECK(chosen(typed, "choice", "accepted\n").text() == "head\naccepted\nend\n");
}

void deletedAnchorsNeverReattachToAnotherRepeat()
{
    auto map = mapped("head\nrepeat\nrepeat\nend\n", "head\nrepeat\nrepeat\nend\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {12, 19}});
    const auto deleted = CorrespondenceMap(map).edited(2, {{12, 19}, {}});
    CHECK(anchorOf(deleted, "choice").state == AnchorState::Invalidated && !anchorOf(deleted, "choice").result);
    const auto whole = map.recomputed(3, "head\nrepeat\nend\n");
    CHECK(anchorOf(whole, "choice").state == AnchorState::Ambiguous && !anchorOf(whole, "choice").result);
}

void insertionAnchorsRespectAffinity()
{
    auto map = mapped("head\ntail\n", "head\ntail\n");
    (void)bound(map, {"choice", SourceSide::Stage2, {5, 5}, InsertionAffinity::Before});
    (void)bound(map, {"after", SourceSide::Stage2, {5, 5}, InsertionAffinity::After});
    const auto edited = CorrespondenceMap(map).edited(2, {{5, 5}, "manual\n"});
    CHECK(anchorOf(edited, "choice").result == ByteRange{5, 5}
          && anchorOf(edited, "after").result == ByteRange{12, 12});
    const auto choice = chosen(edited, "choice", "choice\n");
    CHECK(choice.text() == "head\nchoice\nmanual\ntail\n");
    // Undo and redo restore a map at a fresh revision with the same anchors.
    const auto restored = edited.atRevision(9);
    CHECK(anchorOf(restored, "choice").revision == 9 && anchorOf(restored, "choice").result == ByteRange{5, 5});
}

void theMapChecksEveryTextBeforeTheAlgorithmSeesIt()
{
    const auto algorithm = std::make_shared<WholeTextCorrespondence>();
    const std::string tooLong(kMaxLineBytes + 1, 'a');
    CHECK(rejected([&] { CorrespondenceMap(1, sources("ok\n"), "bad \xff\n", algorithm); }));
    CHECK(rejected([&] { CorrespondenceMap(1, sources("bad \xff\n"), "ok\n", algorithm); }));
    CHECK(rejected([&] { CorrespondenceMap(1, sources(tooLong), "ok\n", algorithm); }));
    CHECK(rejected([&] { CorrespondenceMap(1, sources("ok\n"), tooLong, algorithm); }));
    CHECK(algorithm->calls == 0);
    const CorrespondenceMap map(1, sources("ok\n"), "ok\n", algorithm);
    CHECK(algorithm->calls == 1);
    CHECK(rejected([&] { (void)map.recomputed(2, "bad \xff\n"); }));
    CHECK(rejected([&] { (void)map.recomputed(2, tooLong); }));
    CHECK(algorithm->calls == 1);
    CHECK(map.recomputed(2, "changed\n").text() == "changed\n" && algorithm->calls == 3);
}
}

int main()
{
    test_support::Suite suite("CorrespondenceTests");
    suite.run("immediate and deferred edits admit joined lines before copying",
              immediateAndDeferredEditsCheckJoinedLinesBeforeCopying);
    suite.run("implementations keep the algorithm contract", implementationsKeepTheAlgorithmContract);
    suite.run("correspondence partitions are deterministic for equal changed inserted deleted and ambiguous text",
              partitionsAreDeterministicForEachKindOfPassage);
    suite.run("a long unique text aligns as one equal passage between inserted lines",
              aLongUniqueTextAlignsAsOneEqualPassage);
    suite.run("texts over the cell budget are one ambiguous passage", textsOverTheCellBudgetAreOneAmbiguousPassage);
    suite.run("shared edges align long texts with few changes", sharedEdgesAlignLongTextsWithFewChanges);
    suite.run("far apart changes in a long text align around each change",
              farApartChangesInALongTextAlignAroundEachChange);
    suite.run("equal passages are the matches every optimal alignment shares",
              equalPassagesAreTheMatchesEveryOptimalAlignmentShares);
    suite.run("the map checks every text before the algorithm sees it", theMapChecksEveryTextBeforeTheAlgorithmSeesIt);
    suite.run("a deferred edit keeps the passages around the edited lines",
              aDeferredEditKeepsThePassagesAroundTheEditedLines);
    suite.run("repeated and reordered passages leave anchors ambiguous",
              repeatedAndReorderedPassagesLeaveAnchorsAmbiguous);
    suite.run("equal repeated passages keep the selected occurrence", equalRepeatedPassagesKeepTheSelectedOccurrence);
    suite.run("anchors bind only on character boundaries", anchorsBindOnlyOnCharacterBoundaries);
    suite.run("source-only anchors use the deletion site", sourceOnlyAnchorsUseTheDeletionSite);
    suite.run("anchors follow manual insertions and deletions", anchorsFollowManualInsertionsAndDeletions);
    suite.run("launch text that differs from its source needs confirmation",
              launchTextThatDiffersFromItsSourceNeedsConfirmation);
    suite.run("deleted anchors never reattach to another repeat", deletedAnchorsNeverReattachToAnotherRepeat);
    suite.run("insertion anchors respect affinity", insertionAnchorsRespectAffinity);
    return suite.report();
}
