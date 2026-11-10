// Unit tests for ConnectorBands: which bands show for two laid-out panes and where their ends attach.
#include "TestHarness.h"
#include "TextPane.h"
#include "ui/ConnectorBands.h"
#include <QFont>
#include <QGuiApplication>
#include <QVariantList>
#include <QVariantMap>

using namespace text_pane;

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    test_support::configure(argc, argv);
    test_support::Suite suite("connector-bands");
    suite.run("connector bands clip offscreen and follow layout", [] {
        Pane source(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        Pane result(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        branch_truce::ConnectorBands bands;
        bands.setSide(QStringLiteral("stage2"));
        bands.setSourceBlocks(&source.blocks);
        bands.setResultBlocks(&result.blocks);
        bands.setViewportHeight(source.blocks.yAtLine(4));
        bands.setPassages({QVariantMap{{"side", "stage2"},
                                       {"kind", "changed"},
                                       {"sourceLineBegin", 2},
                                       {"sourceLineEnd", 3},
                                       {"resultLineBegin", 2},
                                       {"resultLineEnd", 3}},
                           QVariantMap{{"side", "stage2"},
                                       {"kind", "changed"},
                                       {"sourceLineBegin", 9},
                                       {"sourceLineEnd", 10},
                                       {"resultLineBegin", 9},
                                       {"resultLineEnd", 10}}});
        bands.setRegions(
            {QVariantMap{{"side", "stage2"}, {"state", "accepted"}, {"sourceLineBegin", 2}, {"sourceLineEnd", 3}}});
        CHECK(bands.bands().size() == 2);
        CHECK(bands.visibleBands().size() == 1);
        CHECK(bands.visibleBands().first().toMap().value("kind").toString() == "accepted");
        bands.setMarkerBlocks({QVariantMap{{"resultLineBegin", 2}, {"resultLineEnd", 4}}});
        bands.setFold(QVariantMap{{"rows", QVariantMap{{"2", QVariantMap{{"last", 3}}}}}});
        const auto folded = bands.bands().first().toMap();
        CHECK(folded.value("kind").toString() == "conflict");
        CHECK(folded.value("resultBegin").toInt() == 2 && folded.value("resultEnd").toInt() == 4);
        bands.setFold({});
        bands.setMarkerBlocks({});
        const qreal scroll = source.blocks.yAtLine(8);
        bands.setSourceScroll(scroll);
        bands.setResultScroll(scroll);
        CHECK(bands.visibleBands().size() == 1);
        CHECK(bands.visibleBands().first().toMap().value("sourceBegin").toInt() == 9);
        const qreal before = bands.visibleBands().first().toMap().value("sourceTop").toReal();
        auto font = source.editor->property("font").value<QFont>();
        font.setPointSizeF(font.pointSizeF() * 1.5);
        CHECK(source.editor->setProperty("font", font));
        QCoreApplication::processEvents();
        CHECK(!bands.visibleBands().isEmpty());
        CHECK(!near(before, bands.visibleBands().first().toMap().value("sourceTop").toReal()));
    });
    suite.run("curved ribbon hover follows full unequal attachment ends", [] {
        Pane source(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        Pane result(QStringLiteral("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"), {});
        branch_truce::ConnectorBands bands;
        bands.setSide("stage2");
        bands.setSourceBlocks(&source.blocks);
        bands.setResultBlocks(&result.blocks);
        bands.setViewportHeight(result.blocks.yAtLine(10));
        bands.setPassages({QVariantMap{{"side", "stage2"},
                                       {"kind", "changed"},
                                       {"sourceLineBegin", 2},
                                       {"sourceLineEnd", 4},
                                       {"resultLineBegin", 7},
                                       {"resultLineEnd", 8}}});
        CHECK(bands.visibleBands().size() == 1);
        const auto band = bands.visibleBands().first().toMap();
        const auto st = band.value("sourceTop").toReal(), sb = band.value("sourceBottom").toReal();
        const auto rt = band.value("resultTop").toReal(), rb = band.value("resultBottom").toReal();
        CHECK(sb > st && rb > rt && rt > sb);
        // At t=1/4, midpoint-control cubic x is 0.296875w and y
        // interpolation is 0.15625. This point is above the straight wedge.
        const qreal width = 40, x = width * 0.296875;
        const qreal top = st + (rt - st) * 0.15625;
        const qreal bottom = sb + (rb - sb) * 0.15625;
        for (const bool left : {true, false}) {
            const auto at = [&](qreal px, qreal py) { return bands.bandAt(left ? px : width - px, py, width, left); };
            CHECK(!at(x, top + (bottom - top) * 0.1).isEmpty());
            CHECK(at(x, top - 1).isEmpty());
            CHECK(!at(0, st).isEmpty() && !at(0, sb).isEmpty());
            CHECK(!at(width, rt).isEmpty() && !at(width, rb).isEmpty());
            CHECK(at(-1, st).isEmpty() && at(width + 1, rt).isEmpty());
            CHECK(at(x, -1).isEmpty());
        }
    });
    return suite.report();
}
