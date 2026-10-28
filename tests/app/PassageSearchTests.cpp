// Unit tests for PassageSearch: its matching, its scopes and its status sentence.
#include "TestHarness.h"
#include "ui/PassageSearch.h"
#include <QCoreApplication>
#include <QSignalSpy>

using branch_truce::PassageSearch;

namespace {
bool matches(const PassageSearch::Match &match, int begin, int count)
{
    return match.begin == begin && match.end == (begin < 0 ? -1 : begin + 6) && match.count == count;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::Suite suite("passage-search");
    const QString text = QStringLiteral("prefix REPEAT middle REPEAT suffix repeat");
    const int first = 7;
    const int second = 21;

    suite.run("TRUCE-0220 a search without a selection takes the first or the last match", [&] {
        CHECK(matches(PassageSearch::search(text, "REPEAT", 0, 0, true), first, 2));
        CHECK(matches(PassageSearch::search(text, "REPEAT", 0, 0, false), second, 2));
        // A cursor without a selection does not move the start.
        CHECK(matches(PassageSearch::search(text, "REPEAT", 30, 30, true), first, 2));
    });
    suite.run("TRUCE-0220 a search continues from a selection that holds the query", [&] {
        CHECK(matches(PassageSearch::search(text, "REPEAT", first, first + 6, true), second, 2));
        CHECK(matches(PassageSearch::search(text, "REPEAT", second, second + 6, false), first, 2));
    });
    suite.run("TRUCE-0220 a search wraps around to the other end", [&] {
        CHECK(matches(PassageSearch::search(text, "REPEAT", second, second + 6, true), first, 2));
        CHECK(matches(PassageSearch::search(text, "REPEAT", first, first + 6, false), second, 2));
        // The only match is found again.
        const QString once = QStringLiteral("one REPEAT only");
        CHECK(matches(PassageSearch::search(once, "REPEAT", 4, 10, true), 4, 1));
        CHECK(matches(PassageSearch::search(once, "REPEAT", 4, 10, false), 4, 1));
    });
    suite.run("TRUCE-0220 matching is case-sensitive", [&] {
        CHECK(matches(PassageSearch::search(text, "Repeat", 0, 0, true), -1, 0));
        CHECK(matches(PassageSearch::search(text, "repeat", 0, 0, true), 35, 1));
        CHECK(matches(PassageSearch::search(text, "", 0, 0, true), -1, 0));
    });
    suite.run("TRUCE-0220 the count leaves out overlapping matches", [] {
        const auto match = PassageSearch::search(QStringLiteral("aaaaa"), "aa", 0, 2, true);
        CHECK(match.begin == 2 && match.end == 4 && match.count == 2);
    });
    suite.run("TRUCE-0220 the scopes list the earlier version only while it is shown", [] {
        PassageSearch search;
        CHECK(search.scopeLabels() == QStringList({"Left source", "Editable result", "Right source"}));
        CHECK(search.scopeIndex() == 0 && search.scope() == "stage2");
        CHECK(search.status() == "Search scope: Left source.");
        search.setScopeIndex(1);
        CHECK(search.scope() == "result" && search.status() == "Search scope: Editable result.");

        QSignalSpy index(&search, &PassageSearch::scopeIndexChanged);
        search.setEarlierShown(true);
        CHECK(search.scopeLabels().size() == 4 && search.scopeLabels().last() == "Earlier version");
        // The selection stays on the result, and a selector that reset itself hears it again.
        CHECK(search.scope() == "result" && search.scopeIndex() == 1 && index.count() == 1);
        search.setScopeIndex(3);
        CHECK(search.scope() == "earlier" && search.status() == "Search scope: Earlier version.");
        search.setEarlierShown(false);
        CHECK(search.scopeLabels().size() == 3 && search.scope() == "result" && search.scopeIndex() == 1);
    });
    suite.run("TRUCE-0220 a search without a query or a pane asks for a passage", [] {
        PassageSearch search;
        const auto found = search.find("stage3", "anything");
        CHECK(found.value("begin").toInt() == -1 && found.value("count").toInt() == 0);
        CHECK(search.status() == "Search scope: Right source. Enter a passage.");
        // Choosing a scope clears the sentence.
        search.setScopeIndex(2);
        CHECK(search.status() == "Search scope: Right source.");
    });
    return suite.report();
}
