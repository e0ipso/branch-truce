// Unit tests for PaneMarks: how passages, regions and marker blocks rank into one mark per line.
#include "TestHarness.h"
#include "ui/PaneMarks.h"
#include <QGuiApplication>
#include <QVariantList>
#include <QVariantMap>

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("pane-marks");
    suite.run("pane marks rank passages, regions and marker blocks", [] {
        branch_truce::PaneMarks marks;
        const QVariantList passages{QVariantMap{{"side", "stage2"},
                                                {"kind", "changed"},
                                                {"resultLineBegin", 1},
                                                {"resultLineEnd", 5},
                                                {"sourceLineBegin", 1},
                                                {"sourceLineEnd", 5}}};
        const QVariantList regions{QVariantMap{{"side", "stage2"},
                                               {"state", "undecided"},
                                               {"mapped", true},
                                               {"markersRemain", true},
                                               {"resultLineBegin", 2},
                                               {"resultLineEnd", 4},
                                               {"sourceLineBegin", 2},
                                               {"sourceLineEnd", 4}}};
        const QVariantList blocks{QVariantMap{{"resultLineBegin", 3},
                                              {"resultLineEnd", 4},
                                              {"folded", true},
                                              {"markerLines",
                                               QVariantList{QVariantMap{{"line", 3}, {"text", "<<<<<<<"}},
                                                            QVariantMap{{"line", 4}, {"text", ">>>>>>>"}}}}}};
        const auto result = marks.marks("result", passages, regions, {}, blocks, {});
        const auto lines = result.value("lines").toMap();
        CHECK(lines.value("1").toString() == "changed");
        CHECK(lines.value("2").toString() == "conflict");
        CHECK(lines.value("3").toString() == "markers");
        CHECK(lines.value("4").toString() == "changed");
        const auto source = marks.marks("stage2", passages, regions, {}, blocks, {}).value("lines").toMap();
        CHECK(source.value("2").toString() == "conflict");
        CHECK(marks.folds(blocks, false).value("hidden").toList().size() == 1);
        CHECK(marks.folds(blocks, true).value("hidden").toList().isEmpty());
        auto ineligible = blocks;
        auto block = ineligible.first().toMap();
        block.insert("folded", false);
        ineligible[0] = block;
        CHECK(marks.folds(ineligible, false).value("hidden").toList().isEmpty());
    });
    suite.run("an undecided nonconflicting contribution retains its passage kind", [] {
        branch_truce::PaneMarks marks;
        const QVariantList passages{QVariantMap{{"side", "stage2"},
                                                {"kind", "changed"},
                                                {"resultLineBegin", 2},
                                                {"resultLineEnd", 3},
                                                {"sourceLineBegin", 2},
                                                {"sourceLineEnd", 3}}};
        const QVariantList regions{QVariantMap{{"side", "stage2"},
                                               {"state", "undecided"},
                                               {"mapped", true},
                                               {"resultLineBegin", 2},
                                               {"resultLineEnd", 3},
                                               {"sourceLineBegin", 2},
                                               {"sourceLineEnd", 3}}};
        for (const auto &pane : {QStringLiteral("stage2"), QStringLiteral("result")})
            CHECK(marks.marks(pane, passages, regions, {}, {}, {}).value("lines").toMap().value("2").toString()
                  == "changed");
        CHECK(regions.first().toMap().value("state").toString() == "undecided");
    });
    return suite.report();
}
