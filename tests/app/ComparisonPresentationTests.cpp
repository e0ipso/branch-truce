// Observable joined rows, result actions and rejection of stale comparison targets.
#include "TestHarness.h"
#include "app/ComparisonPresentation.h"
#include "ui/PaneMarks.h"
#include <QCoreApplication>
#include <QSignalSpy>

using namespace branch_truce;
namespace {
ComparisonContribution contribution(const char *id, const char *side, const char *scope, int first = 2, int end = 4)
{
    ComparisonContribution c;
    c.id = id;
    c.side = side;
    c.scope = scope;
    c.section = "section";
    c.label = QString::fromLatin1(id);
    c.state = "undecided";
    c.sourceLineBegin = c.resultLineBegin = first;
    c.sourceLineEnd = c.resultLineEnd = end;
    c.available = c.combinationAvailable = c.mapped = true;
    return c;
}
ComparisonPresentation::Data data()
{
    ComparisonPresentation::Data d;
    d.fileId = "file";
    d.revision = "1";
    d.correspondence = "alignment";
    d.contributions = {contribution("left-line", "stage2", "line", 3, 4), contribution("right", "stage3", "section"),
                       contribution("left", "stage2", "section"), contribution("right-line", "stage3", "line", 2, 3)};
    return d;
}
}
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    test_support::Suite suite("ComparisonPresentationTests");
    suite.run("source rows join contributions by identity regardless of input position", [] {
        ComparisonPresentation p;
        auto d = data();
        auto detached = contribution("detached", "stage2", "line", 7, 7);
        detached.section = "other";
        d.contributions.prepend(detached);
        p.publish(d);
        CHECK(p.leftRows().size() == 2 && p.rightRows().size() == 1);
        CHECK(p.leftRows()[0].choice.id == "left" && p.leftRows()[0].extras[0].id == "left-line");
        CHECK(p.rightRows()[0].choice.id == "right" && p.rightRows()[0].extras[0].id == "right-line");
        CHECK(p.leftRows()[1].line == 7 && p.leftRows()[1].end == 7 && p.leftRows()[1].extras.isEmpty());
        CHECK(p.targetAt("stage2", 7).value("contributionId") == "detached");
        CHECK(p.targetAt("result", 7).value("contributionId") == "detached");
        CHECK(p.choiceTarget("missing").isEmpty());
    });
    suite.run("result sections retain one-sided mappings, decisions and folded combination labels", [] {
        ComparisonPresentation p;
        auto d = data();
        d.contributions[2].mapped = false;
        d.markerBlocks = {
            QVariantMap{{"resultLineBegin", 1},
                        {"resultLineEnd", 6},
                        {"folded", true},
                        {"sections", QStringList{"section"}},
                        {"markerLines", QVariantList{QVariantMap{{"line", 1}}, QVariantMap{{"line", 5}}}}}};
        p.publish(d);
        CHECK(p.resultSections(false).value("combinations").toMap().value("1").toList()[0].toMap().value("label")
              == "left and right");
        CHECK(p.resultSections(true).value("combinations").toMap().isEmpty());
        CHECK(p.activeConflict(5, {}, p.generation()).value("section") == "section");
        d.contributions[2].state = "accepted";
        d.contributions[1].state = "ignored";
        p.publish(d);
        const auto decisions = p.resultSections(false).value("decisions").toMap();
        CHECK(decisions.value("1").toMap().value("label") == "Left accepted, Right ignored");
        CHECK(decisions.value("1").toMap().value("state") == "accepted");
        CHECK(p.activeConflict(2, {}, p.generation()).isEmpty());
        d.contributions[1].state = "manual";
        d.contributions.removeAt(2);
        p.publish(d);
        CHECK(p.resultSections(true).value("decisions").toMap().value("2").toMap().value("label")
              == "Right edited by hand");
        d.contributions[1].mapped = false;
        p.publish(d);
        CHECK(p.resultSections(false).value("decisions").toMap().isEmpty());
    });
    suite.run("revision and correspondence swaps reject cached decorations and navigation ranges", [] {
        ComparisonPresentation p;
        PaneMarks marks;
        marks.setPresentation(&p);
        auto d = data();
        p.publish(d);
        const auto old = p.choiceTarget("left");
        CHECK(p.accepts(old));
        CHECK(!marks.current("stage2", old, p.fileId(), {}, p.contributions(), nullptr).isEmpty());
        auto navigation = old;
        navigation.remove("generation");
        for (const auto &change : {QStringLiteral("alignment"), QStringLiteral("revision"), QStringLiteral("file")}) {
            d = data();
            if (change == "alignment") d.correspondence = "next";
            if (change == "revision") d.revision = "2";
            if (change == "file") d.fileId = "other";
            p.publish(d);
            CHECK(!p.accepts(old) && !p.accepts(navigation));
            CHECK(marks.current("stage2", navigation, p.fileId(), {}, p.contributions(), nullptr).isEmpty());
            CHECK(p.activeConflict(2, old, old.value("generation").toString()).isEmpty());
        }
    });
    suite.run("unchanged source commands and result offers keep their notification identity", [] {
        ComparisonPresentation p;
        PaneMarks marks;
        CHECK(marks.fold().contains("hidden") && marks.sections().contains("combinations"));
        CHECK(marks.lineMarks().value("result").toMap().contains("lines"));
        marks.setPresentation(&p);
        auto d = data();
        p.publish(d);
        QSignalSpy left(&p, &ComparisonPresentation::leftRowsChanged),
            right(&p, &ComparisonPresentation::rightRowsChanged);
        QSignalSpy sections(&marks, &PaneMarks::sectionsChanged), folds(&marks, &PaneMarks::foldChanged);
        const auto leftRows = p.leftRows(), rightRows = p.rightRows();
        d.revision = "2";
        p.publish(d);
        CHECK(p.leftRows() == leftRows && p.rightRows() == rightRows);
        CHECK(left.isEmpty() && right.isEmpty() && sections.isEmpty() && folds.isEmpty());
        p.publish({});
        CHECK(p.contributions().isEmpty() && p.leftRows().isEmpty() && p.rightRows().isEmpty());
        CHECK(!p.accepts({}));
        CHECK(left.size() == 1 && right.size() == 1);
    });
    return suite.report();
}
