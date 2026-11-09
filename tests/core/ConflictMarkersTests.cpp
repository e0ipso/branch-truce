// Conflict marker recognition: marker lines, block assembly, known source text and marker length.
#include "TestHarness.h"
#include "core/ConflictMarkers.h"
#include "core/ResourceLimits.h"
#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace branch_truce;

namespace {
std::string block(std::size_t size)
{
    return std::string(size, '<') + " ours\nleft\n" + std::string(size, '=') + "\nright\n" + std::string(size, '>')
        + " theirs\n";
}

void markerSizeFollowsGitsReadingOfTheAttribute()
{
    const std::pair<const char *, std::optional<std::size_t>> cases[] = {{"32", 32},
                                                                         {" 32", 32},
                                                                         {"+9", 9},
                                                                         {"32abc", 32},
                                                                         {"", 7},
                                                                         {"set", 7},
                                                                         {"unset", 7},
                                                                         {"0", 7},
                                                                         {"-5", 7},
                                                                         {"1024", 1024},
                                                                         {"1025", {}},
                                                                         {"99999999999999999999999", {}},
                                                                         {"-99999999999999999999", 7}};
    for (const auto &[attribute, size] : cases) CHECK(conflictMarkerSize(attribute) == size);
    CHECK(kMaxConflictMarkerSize == 1024 && kDefaultConflictMarkerSize == 7);
}

void longerMarkersAreRecognizedOnlyAtTheirLength()
{
    const auto scan = scanMarkers(block(32), {.markerSize = 32});
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
    CHECK(scan.blocks.front().separator.line == 2 && scan.blocks.front().end.line == 4);
    CHECK(scanMarkers(block(32)) == MarkerScan{});
    CHECK(scanMarkers(block(7), {.markerSize = 32}) == MarkerScan{});
    CHECK(scanMarkers(block(7)).blocks.size() == 1);
}

void aCompleteBlockIsOneTypedRange()
{
    const std::string text = "a\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\nz\n";
    const auto scan = scanMarkers(text);
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1 && scan.count() == 1);
    const auto &block = scan.blocks.front();
    CHECK(block.begin == (MarkerLine{MarkerKind::Begin, 1, {2, 15}}));
    CHECK(!block.base);
    CHECK(block.separator == (MarkerLine{MarkerKind::Separator, 3, {20, 28}}));
    CHECK(block.end == (MarkerLine{MarkerKind::End, 5, {34, 49}}));
    CHECK(text.substr(block.end.bytes.begin, block.end.bytes.end - block.end.bytes.begin) == ">>>>>>> theirs\n");
    CHECK(scan.first() == block.begin);
}

void aBlockWithABaseSectionKeepsTheBaseLine()
{
    const std::string text = "<<<<<<< ours\nleft\n||||||| base\nbase\n=======\nright\n>>>>>>> theirs";
    const auto scan = scanMarkers(text);
    CHECK(scan.orphans.empty() && scan.blocks.size() == 1);
    const auto &block = scan.blocks.front();
    CHECK(block.base == (MarkerLine{MarkerKind::Base, 2, {18, 31}}));
    CHECK(block.separator.line == 4 && block.end.line == 6);
    // The last line has no line ending.
    CHECK(block.end.bytes == (ByteRange{50, text.size()}));
}

void eachMarkerLineAloneIsAnOrphan()
{
    const std::array<std::pair<std::string, MarkerKind>, 4> lines{{{"<<<<<<< ours", MarkerKind::Begin},
                                                                   {"||||||| base", MarkerKind::Base},
                                                                   {"=======", MarkerKind::Separator},
                                                                   {">>>>>>> theirs", MarkerKind::End}}};
    for (const auto &[line, kind] : lines) {
        std::string text = "before\n";
        text += line;
        text += "\nafter\n";
        const auto scan = scanMarkers(text);
        CHECK(scan.blocks.empty() && scan.count() == 1);
        CHECK(scan.orphans == (std::vector<MarkerLine>{{kind, 1, {7, 8 + line.size()}}}));
    }
}

void markerLikeTextThatIsNotAWholeMarkerIsIgnored()
{
    const std::string text = "x <<<<<<< ours\n  =======\n<<<<<<<< eight\n======== heading\n"
                             ">>>>>>>theirs\n|||||| six\nend >>>>>>> theirs\n";
    CHECK(scanMarkers(text) == MarkerScan{});
}

void aSeparatorFollowedByACarriageReturnIsNotAMarkerLine()
{
    // Adapters convert line endings, so the engine never stores a carriage return.
    const std::string text = "<<<<<<< ours\r\nleft\r\n=======\r\nright\r\n>>>>>>> theirs\r\n";
    const auto scan = scanMarkers(text);
    CHECK(scan.blocks.empty() && scan.orphans.size() == 2);
    CHECK(scan.orphans.front().line == 0 && scan.orphans.back().line == 4);
}

void brokenAndNestedBlocksLeaveEveryMarkerLineCounted()
{
    const std::string text = "<<<<<<< a\n<<<<<<< b\nleft\n=======\nright\n>>>>>>> b\n"
                             "=======\n>>>>>>> a\n||||||| late\n";
    const auto scan = scanMarkers(text);
    CHECK(scan.blocks.size() == 1 && scan.blocks.front().begin.line == 1);
    std::vector<std::size_t> orphanLines;
    orphanLines.reserve(scan.orphans.size());
    for (const auto &line : scan.orphans) orphanLines.push_back(line.line);
    CHECK(orphanLines == (std::vector<std::size_t>{0, 6, 7, 8}));
    const auto first = scan.first();
    CHECK(scan.count() == 5 && first && first->line == 0);
}

void markerLikeLinesInsideKnownSourceTextAreSkipped()
{
    const std::string text = "Title\n=======\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\n";
    const auto plain = scanMarkers(text);
    CHECK(plain.blocks.size() == 1 && plain.orphans.size() == 1 && plain.orphans.front().line == 1);
    // Unordered, overlapping ranges merge, and the heading underline inside them is content.
    const auto filtered = scanMarkers(text, {.known = {{6, 14}, {0, 10}}});
    CHECK(filtered.orphans.empty() && filtered.blocks == plain.blocks);
    // Only a range covering the whole line makes it content.
    CHECK(scanMarkers(text, {.known = {{6, 13}}}) == plain);
    // Blocks are assembled from the lines left over.
    const std::string inside = "<<<<<<< ours\nTitle\n=======\n=======\nright\n>>>>>>> theirs\n";
    CHECK(scanMarkers(inside).orphans.size() == 1 && scanMarkers(inside).blocks.front().separator.line == 2);
    const auto assembled = scanMarkers(inside, {.known = {{13, 27}}});
    CHECK(assembled.orphans.empty() && assembled.blocks.size() == 1 && assembled.blocks.front().separator.line == 3);
}

void earlierTextIsContentOnlyInsideABaseRegion()
{
    const std::string text = "<<<<<<< ours\nleft\n||||||| base\nTitle\n=======\n=======\nright\n>>>>>>> theirs\n";
    const auto plain = scanMarkers(text);
    CHECK(plain.blocks.front().separator.line == 4 && plain.orphans.size() == 1 && plain.orphans.front().line == 5);
    // The underline lies in the earlier text, so the next separator closes the base.
    const auto assembled = scanMarkers(text, {.earlier = {{31, 45}}});
    CHECK(assembled.orphans.empty() && assembled.blocks.size() == 1);
    const auto &closing = assembled.blocks.front();
    CHECK(closing.base && closing.base->line == 2 && closing.separator.line == 5);
    // Earlier text before the base line or after the separator is not content.
    const std::string heading = "Title\n=======\n<<<<<<< ours\nleft\n=======\nright\n>>>>>>> theirs\n";
    CHECK(scanMarkers(heading, {.earlier = {{0, 14}}}) == scanMarkers(heading));
    CHECK(scanMarkers(heading, {.earlier = {{0, heading.size()}}}) == scanMarkers(heading));
    CHECK(scanMarkers(text, {.earlier = {{0, 31}, {31, 45}, {53, text.size()}}}) == assembled);
}

void aBaseEndingInASeparatorLineClosesWhereRightTextFollows()
{
    const std::string text = "<<<<<<< ours\nleft\n||||||| base\nTitle\n=======\n=======\nright\n>>>>>>> theirs\n";
    const ByteRange title{31, 37}, lines{37, 53}, right{53, 59};
    // Without right-side text after the second line, the first one closes the base.
    CHECK(scanMarkers(text, {.earlier = {title}, .uncertain = {lines}}) == scanMarkers(text));
    CHECK(scanMarkers(text, {.known = {right}, .earlier = {title}, .right = {right}})
          == scanMarkers(text, {.known = {right}}));
    const auto assembled =
        scanMarkers(text, {.known = {right}, .earlier = {title}, .uncertain = {lines}, .right = {right}});
    CHECK(assembled.orphans.empty() && assembled.blocks.size() == 1);
    const auto &closing = assembled.blocks.front();
    CHECK(closing.base && closing.base->line == 2 && closing.separator.line == 5);
    // The end marker shows an empty right region just as well.
    const std::string empty = "<<<<<<< ours\nleft\n||||||| base\nTitle\n=======\n=======\n>>>>>>> theirs\n";
    const auto closed = scanMarkers(empty, {.earlier = {title}, .uncertain = {lines}});
    CHECK(closed.orphans.empty() && closed.blocks.front().separator.line == 5);
    // A separator line the earlier text cannot hold still closes the base.
    CHECK(scanMarkers(text, {.known = {right}, .earlier = {title}, .uncertain = {{45, 53}}, .right = {right}})
          == scanMarkers(text, {.known = {right}}));
    // Outside a base region nothing changes.
    const std::string twoWay = "<<<<<<< ours\nleft\n=======\n=======\nright\n>>>>>>> theirs\n";
    CHECK(scanMarkers(twoWay, {.uncertain = {{18, 34}}, .right = {{34, 40}}}) == scanMarkers(twoWay));
}
}

int main()
{
    test_support::Suite suite("ConflictMarkersTests");
    suite.run("the marker length follows Git's reading of conflict-marker-size",
              markerSizeFollowsGitsReadingOfTheAttribute);
    suite.run("longer markers are recognized only at their length", longerMarkersAreRecognizedOnlyAtTheirLength);
    suite.run("a complete block is one typed range with line and byte bounds", aCompleteBlockIsOneTypedRange);
    suite.run("a block with a base section keeps the base line", aBlockWithABaseSectionKeepsTheBaseLine);
    suite.run("each marker line alone is an orphan", eachMarkerLineAloneIsAnOrphan);
    suite.run("marker-like text that is not a whole marker is ignored", markerLikeTextThatIsNotAWholeMarkerIsIgnored);
    suite.run("a separator followed by a carriage return is not a marker line",
              aSeparatorFollowedByACarriageReturnIsNotAMarkerLine);
    suite.run("broken and nested blocks leave every marker line counted",
              brokenAndNestedBlocksLeaveEveryMarkerLineCounted);
    suite.run("scanMarkers skips marker-like lines inside known source text",
              markerLikeLinesInsideKnownSourceTextAreSkipped);
    suite.run("scanMarkers counts earlier text as content only inside a base region",
              earlierTextIsContentOnlyInsideABaseRegion);
    suite.run("scanMarkers closes a base ending in a separator line where right text follows",
              aBaseEndingInASeparatorLineClosesWhereRightTextFollows);
    return suite.report();
}
