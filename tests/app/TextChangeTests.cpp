#include "app/TextChange.h"
#include "TestHarness.h"
#include <QString>
#include <vector>

using namespace branch_truce;

namespace {
bool same(const TextChange &change, qsizetype begin, qsizetype oldEnd, qsizetype newEnd)
{
    return change.begin == begin && change.oldEnd == oldEnd && change.newEnd == newEnd;
}

QString units(std::initializer_list<char16_t> values)
{
    QString result;
    for (const auto value : values) result.append(QChar(value));
    return result;
}
}

int main()
{
    test_support::Suite suite("text-change");

    suite.run("one replaced range keeps the common prefix and suffix", [] {
        CHECK(same(changedRange(QStringLiteral("abc"), QStringLiteral("abXc")), 2, 2, 3));
        CHECK(same(changedRange(QStringLiteral("abc"), QStringLiteral("ac")), 1, 2, 1));
        CHECK(same(changedRange(QStringLiteral("abc"), QStringLiteral("abc")), 3, 3, 3));
    });

    // TRUCE-0202: a change never splits a surrogate pair.
    suite.run("a change inside a pair's low surrogate widens back to its high surrogate", [] {
        // U+1F600 and U+1F601 share the high surrogate D83D.
        const auto old = units({u'a', 0xD83D, 0xDE00, u'b'});
        const auto next = units({u'a', 0xD83D, 0xDE01, u'b'});
        CHECK(same(changedRange(old, next), 1, 3, 3));
    });

    suite.run("a change inside a pair's high surrogate widens forward to its low surrogate", [] {
        // U+10000 and U+10400 share the low surrogate DC00.
        const auto old = units({u'x', 0xD800, 0xDC00, u'y'});
        const auto next = units({u'x', 0xD801, 0xDC00, u'y'});
        CHECK(same(changedRange(old, next), 1, 3, 3));
    });

    suite.run("unchanged lines split one outer range into separate changes", [] {
        const auto old = QStringLiteral("one\nkeep\ntwo\n");
        const auto next = QStringLiteral("1\nkeep\n2\n");
        CHECK(same(changedRange(old, next), 0, 12, 8));
        const auto changes = changedRanges(old, next);
        CHECK(changes.size() == 2);
        CHECK(same(changes[0], 0, 3, 1));
        CHECK(same(changes[1], 9, 12, 8));
        CHECK(changedRanges(old, old).empty());
    });

    suite.run("offsets before, inside and after a replacement", [] {
        // Two units at 2 replaced by three.
        const std::vector<TextChange> changes{{2, 4, 5}};
        CHECK(changedOffset(changes, 1) == 1);
        CHECK(changedOffset(changes, 2) == 2);
        CHECK(changedOffset(changes, 3) == 5);
        CHECK(changedOffset(changes, 4) == 5);
        CHECK(changedOffset(changes, 6) == 7);
        CHECK(changedOffset({}, 7) == 7);
    });

    suite.run("an offset at an insertion goes to its end", [] {
        const std::vector<TextChange> changes{{2, 2, 4}};
        CHECK(changedOffset(changes, 1) == 1);
        CHECK(changedOffset(changes, 2) == 4);
        CHECK(changedOffset(changes, 3) == 5);
    });

    suite.run("offsets between and after split changes follow each change", [] {
        const auto changes = changedRanges(QStringLiteral("one\nkeep\ntwo\n"), QStringLiteral("1\nkeep\n2\n"));
        CHECK(changedOffset(changes, 5) == 3);
        CHECK(changedOffset(changes, 10) == 8);
        CHECK(changedOffset(changes, 13) == 9);
    });

    return suite.report();
}
