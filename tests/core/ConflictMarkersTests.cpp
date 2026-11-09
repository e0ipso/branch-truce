// Conflict marker recognition: marker lines, block assembly and marker length.
#include "TestHarness.h"
#include "core/ConflictMarkers.h"
#include "core/ResourceLimits.h"
#include <string>

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
}

int main()
{
    test_support::Suite suite("ConflictMarkersTests");
    suite.run("the marker length follows Git's reading of conflict-marker-size",
              markerSizeFollowsGitsReadingOfTheAttribute);
    suite.run("longer markers are recognized only at their length", longerMarkersAreRecognizedOnlyAtTheirLength);
    return suite.report();
}
