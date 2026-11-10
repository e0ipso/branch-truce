// What the panes paint around the text: gutter glyphs, provenance, connector bands and their handles.
#include "ComparisonCases.h"
#include "ComparisonEditor.h"
#include "ComparisonGeometry.h"
#include "ComparisonMarks.h"
#include "ComparisonTextProbes.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"
#include "fixtures/WarningCapture.h"

using test_support::await;
using test_support::commandResult;

using namespace comparison_scene;

void comparison_scene::addMarkCases(Groups groups)
{
    auto &scenes = groups.scenes;
    scenes.run("every coloured line has a gutter glyph of its kind", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller,
                        test_support::choiceFile("header\nleft first\nshared\ntail\n",
                                                 "header\nright first\nshared\nright extra\ntail\n"));
        const QMap<QString, QString> glyphs{{"added", "+"},
                                            {"changed", "~"},
                                            {"conflict", "!"},
                                            {"manual", "m"},
                                            {"accepted", QString(QChar(0x2713))},
                                            {"ignored", QString(QChar(0x00d7))},
                                            {"removed", QString(QChar(0x2212))}};
        // Each coloured line has one state glyph. A gap on that same line
        // is described by the glyph instead of adding a second symbol.
        struct Painted {
            QSet<QString> kinds;
            int unmarked = 0;
        };
        const auto painted = [&](const std::string &pane) {
            Painted found;
            QMap<QPair<int, QString>, QString> shown;
            for (auto *glyph : fixture.item((pane + "GutterMarkerSlot").c_str())->childItems()) {
                if (glyph->isVisible() && glyph->property("kind").isValid())
                    shown.insert({glyph->property("line").toInt(), glyph->property("kind").toString()},
                                 glyph->property("text").toString());
            }
            for (const auto *suffix : {"BackgroundsLines", "BackgroundsGaps"}) {
                for (auto *fill : fixture.item((pane + suffix).c_str())->childItems()) {
                    if (!fill->isVisible() || !fill->property("line").isValid()) continue;
                    const auto kind = fill->property("kind").toString();
                    found.kinds.insert(kind);
                    const auto line = fill->property("line").toInt();
                    const auto glyph = shown.value({line, kind});
                    if (QString::fromLatin1(suffix) == "BackgroundsGaps") {
                        const auto lineKind =
                            qmlValue(fixture.item((pane + "Gutter").c_str()), QStringLiteral("lineKind(%1)").arg(line))
                                .toString();
                        if (!lineKind.isEmpty()) {
                            const auto description = qmlValue(fixture.item((pane + "Gutter").c_str()),
                                                              QStringLiteral("lineDescription(%1)").arg(line))
                                                         .toString()
                                                         .contains(QStringLiteral("Unmatched lines at this boundary"));
                            if (shown.value({line, lineKind}) != glyphs.value(lineKind)
                                || shown.contains({line, QStringLiteral("removed")}) || !description)
                                ++found.unmarked;
                            continue;
                        }
                    }
                    if (glyph.isEmpty() || glyph != glyphs.value(kind)) ++found.unmarked;
                }
            }
            return found;
        };
        const auto expect = [&](const std::string &pane, const QSet<QString> &kinds) {
            settleFor(pane + " diff colours and glyphs", [&] {
                const auto found = painted(pane);
                return found.kinds == kinds && found.unmarked == 0;
            });
            CHECK(painted(pane).unmarked == 0);
        };
        // At launch every region is undecided; the right source's extra line is a gap in the result.
        expect("stage2", {});
        expect("result", {"changed", "removed"});
        expect("stage3", {"changed", "added"});
        auto *slot = fixture.item("resultGutterMarkerSlot");
        int gapGlyphs = 0;
        for (auto *glyph : slot->childItems()) {
            if (glyph->isVisible() && glyph->property("kind").toString() == "removed") {
                ++gapGlyphs;
                CHECK(glyph->x() + glyph->width() < slot->width());
            }
        }
        int describedGaps = 0;
        for (auto *fill : fixture.item("resultBackgroundsGaps")->childItems()) {
            if (!fill->isVisible() || !fill->property("line").isValid()) continue;
            const auto line = fill->property("line").toInt();
            if (!qmlValue(fixture.item("resultGutter"), QStringLiteral("lineKind(%1)").arg(line)).toString().isEmpty())
                ++describedGaps;
        }
        CHECK(gapGlyphs + describedGaps > 0);
        // Taking the left side decides every region, and typing in an accepted line marks it manual.
        // The edited region shows its passage colours again; the right's extra line stays ignored.
        const auto revision = [&] { return QString::number(fixture.file().current().id); };
        CHECK(test_support::previewResult(
            controller,
            controller.choices()->previewWholeFile("choices", revision(), ChoicePreviewFlow::Side::LeftSide)));
        CHECK(commandResult(controller, controller.choices()->applyWholeFile()));
        const auto at = static_cast<int>(fixture.text().indexOf("left first"));
        CHECK(commandResult(controller, controller.commands()->editResult("choices", revision(), at, at + 4, "LEFT")));
        expect("stage2", {"changed"});
        expect("result", {"manual", "removed"});
        // the right's extra line is ignored. Its glyph stays, its text
        // takes the disabled colour, and it gets no background fill.
        expect("stage3", {"changed"});
        const auto ignoredTinted = [&] {
            const auto stage3 = paintedMarks(fixture, "stage3");
            const auto ignored = stage3.glyphs.keys(QStringLiteral("ignored"));
            return !ignored.isEmpty() && stage3.tinted.keys() == ignored
                && std::none_of(ignored.begin(), ignored.end(),
                                [&](int line) { return stage3.backgrounds.contains(line); });
        };
        settleFor("stage3 ignored tint", ignoredTinted);
        CHECK(ignoredTinted());
    });
    scenes.run("gutter delegates detach without anchor errors", [] {
        const test_support::WarningCapture anchorErrors([](const QString &message) {
            return message.contains(QStringLiteral("LineGutter.qml"))
                && message.contains(QStringLiteral("Cannot read property 'right' of null"));
        });
        {
            Application controller(test_support::windowConfig());
            Fixture fixture(controller);
            settlePanes(fixture);
            auto *gutter = fixture.item("resultGutter");
            CHECK(qmlValue(gutter, QStringLiteral("rowSlots.count")).toInt() > 0);
            qmlValue(gutter, QStringLiteral("rowSlots.clear()"));
            CHECK(qmlValue(gutter, QStringLiteral("rowSlots.count")).toInt() == 0);
            qmlValue(gutter, QStringLiteral("rows = []"));
            qmlValue(gutter, QStringLiteral("invalidate()"));
            settleFor("gutter rows restored",
                      [&] { return qmlValue(gutter, QStringLiteral("rowSlots.count")).toInt() > 0; });
        }
        // The scene's delegates detach as the deferred deletes run.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents();
        CHECK_EQ(anchorErrors.count(), 0);
    });
    scenes.run(
        "a hand edit outside every region shows the neutral background, one m glyph and inspectable provenance", [] {
            Application controller(test_support::windowConfig());
            Fixture fixture(controller,
                            test_support::choiceFile("header\nleft first\nshared\ntail\n",
                                                     "header\nright first\nshared\nright extra\ntail\n"));
            const auto revision = [&] { return QString::number(fixture.file().current().id); };
            CHECK(test_support::previewResult(
                controller,
                controller.choices()->previewWholeFile("choices", revision(), ChoicePreviewFlow::Side::LeftSide)));
            CHECK(commandResult(controller, controller.choices()->applyWholeFile()));
            // Line 1 is the header both sides share, so no choice region covers it.
            const auto header = [&](const QString &background, const QString &glyph, const QString &letter) {
                const auto marks = paintedMarks(fixture, "result");
                return marks.backgrounds.value(1) == background && marks.glyphs.value(1) == glyph
                    && marks.letters.value(1) == letter;
            };
            settleFor("the shared header before the edit", [&] { return header({}, {}, "B"); });
            CHECK(commandResult(controller, controller.commands()->editResult("choices", revision(), 0, 6, "HEADER")));
            settleFor("the hand-edited header", [&] { return header("manual", "manual", "m"); });
            CHECK(header("manual", "manual", "m"));
            const auto edited = fixture.file().correspondence()->handEditedRanges();
            CHECK((edited == std::vector<ByteRange>{{0, 6}}));
            // Undo and redo take the marking with the revision.
            CHECK(commandResult(controller, controller.commands()->undo("choices", revision())));
            settleFor("the header after undo", [&] { return header({}, {}, "B"); });
            CHECK(header({}, {}, "B"));
            CHECK(commandResult(controller, controller.commands()->redo("choices", revision())));
            settleFor("the header after redo", [&] { return header("manual", "manual", "m"); });
            CHECK(header("manual", "manual", "m"));
            // The fill is the theme's neutral background.
            int filled = 0;
            for (auto *fill : fixture.item("resultBackgroundsLines")->childItems()) {
                if (!fill->isVisible() || fill->property("line").toInt() != 1) continue;
                ++filled;
                CHECK(fill->property("color").value<QColor>()
                      == qmlValue(fill, QStringLiteral("Kirigami.Theme.neutralBackgroundColor")).value<QColor>());
            }
            CHECK(filled == 1);
        });
    {
        // The batch fixture has a left and a right region on lines 2, 4 and 6, and
        // its result is the common earlier version. The result's letters name the
        // sources each line equals; line 6 equals neither.
        const QMap<int, QString> launchLetters{{1, "B"}, {2, "R"}, {3, "B"}, {4, "L"}, {5, "B"}, {7, "B"}};
        auto leftLetters = launchLetters;
        leftLetters.insert(2, "L");
        // One scoped choice on the left region at line 2: the action, the states the
        // left and right regions there end in, and what each pane then paints. The
        // regions on lines 4 and 6 stay undecided; only line 6 is a conflict. A
        // decided side is marked accepted or ignored on its source lines.
        struct Choice {
            QString name;
            ChoicePreviewFlow::Action action;
            QString leftState;
            QString rightState;
            bool resultChanges;
            PaneMarks stage2;
            PaneMarks result;
            PaneMarks stage3;
        };
        const std::array<Choice, 3> choices{{
            {"accept", ChoicePreviewFlow::Action::Accept, "accepted", "undecided", true,
             regionMarks("accepted", {}, "conflict"), regionMarks("accepted", "changed", "conflict", leftLetters),
             regionMarks("changed", "changed", "conflict")},
            {"ignore", ChoicePreviewFlow::Action::Ignore, "ignored", "undecided", false,
             regionMarks("ignored", {}, "conflict"), regionMarks("changed", "changed", "conflict", launchLetters),
             regionMarks({}, "changed", "conflict")},
            {"resolve", ChoicePreviewFlow::Action::ResolveUsingSide, "accepted", "ignored", true,
             regionMarks("accepted", {}, "conflict"), regionMarks("accepted", "changed", "conflict", leftLetters),
             regionMarks("ignored", "changed", "conflict")},
        }};
        for (const auto &choice : choices) {
            // regionMarks gives an ignored line a tinted text and no background,
            // so undo also checks that the tint goes.
            scenes.run(
                "a scoped " + choice.name.toStdString()
                    + " repaints all three panes and inspected provenance, and undo restores them",
                [choice, launchLetters] {
                    Application controller(test_support::windowConfig());
                    Fixture fixture(controller);
                    settlePanes(fixture);
                    expectMarks(fixture, "at launch", "stage2", regionMarks("changed", {}, "conflict"));
                    expectMarks(fixture, "at launch", "result",
                                regionMarks("changed", "changed", "conflict", launchLetters));
                    expectMarks(fixture, "at launch", "stage3", regionMarks({}, "changed", "conflict"));
                    const auto before = fixture.text();
                    const auto &target =
                        test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
                    CHECK(test_support::previewResult(
                        controller,
                        controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                            QString::fromStdString(target.source.contributionId),
                                                            choice.action)));
                    CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
                    CHECK((fixture.text() != before) == choice.resultChanges);
                    if (choice.resultChanges) CHECK(fixture.text().contains(QStringLiteral("\nleft only\n")));
                    // Both scopes of a region share its decisions, so they end in the same state.
                    int chosen = 0;
                    for (const auto &entry : controller.selectedFile()->comparison()->contributions()) {
                        const auto &region = entry;
                        const bool left = region.side == "stage2";
                        const bool here = region.sourceLineBegin == 2;
                        chosen += left && here;
                        const auto expected = !here ? QStringLiteral("undecided")
                            : left                  ? choice.leftState
                                                    : choice.rightState;
                        if (region.state != expected)
                            throw std::runtime_error(region.id.toStdString() + " is " + region.state.toStdString()
                                                     + ", not " + expected.toStdString());
                    }
                    CHECK(chosen == 2);
                    if (choice.name == "resolve")
                        CHECK(qmlValue(fixture.item("comparisonView"),
                                       QStringLiteral("resultSections.decisions[2].label"))
                                  .toString()
                              == "Left accepted, Right ignored");
                    const auto when = "after " + choice.name.toStdString();
                    expectMarks(fixture, when, "stage2", choice.stage2);
                    expectMarks(fixture, when, "result", choice.result);
                    expectMarks(fixture, when, "stage3", choice.stage3);
                    // Undo returns every pane to the undecided look of launch.
                    CHECK(commandResult(
                        controller,
                        controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
                    CHECK(fixture.text() == before);
                    const auto undone = "after undoing " + choice.name.toStdString();
                    expectMarks(fixture, undone, "stage2", regionMarks("changed", {}, "conflict"));
                    expectMarks(fixture, undone, "result",
                                regionMarks("changed", "changed", "conflict", launchLetters));
                    expectMarks(fixture, undone, "stage3", regionMarks({}, "changed", "conflict"));
                });
        }
    }
    scenes.run("each non-equal passage gets a band from its source lines to its result lines", [] {
        // In the batch fixture the left source differs from the result on lines 2
        // and 6 and the right source on lines 4 and 6, and every region is undecided.
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        settlePanes(fixture);
        const std::vector<std::string> left{"changed 2-3>2-3", "conflict 6-7>6-7"};
        const std::vector<std::string> right{"changed 4-5>4-5", "conflict 6-7>6-7"};
        expectBands(fixture, "at launch", "stage2", left);
        expectBands(fixture, "at launch", "stage3", right);
        // A scoped choice on the left region at line 2 redraws the bands with the
        // passages, and undo restores them. Acceptance keeps a positive band on
        // the changed-against-earlier range; the right line now differs from it.
        struct Choice {
            QString name;
            ChoicePreviewFlow::Action action;
            std::vector<std::string> left;
            std::vector<std::string> right;
        };
        const std::array<Choice, 2> choices{{
            {"accept",
             ChoicePreviewFlow::Action::Accept,
             {"accepted 2-3>2-3", "conflict 6-7>6-7"},
             {"changed 2-3>2-3", "changed 4-5>4-5", "conflict 6-7>6-7"}},
            {"ignore", ChoicePreviewFlow::Action::Ignore, {"ignored 2-3>2-3", "conflict 6-7>6-7"}, right},
        }};
        for (const auto &choice : choices) {
            const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
            CHECK(test_support::previewResult(
                controller,
                controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                    QString::fromStdString(target.source.contributionId),
                                                    choice.action)));
            CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
            const auto when = "after " + choice.name.toStdString();
            expectBands(fixture, when, "stage2", choice.left);
            expectBands(fixture, when, "stage3", choice.right);
            CHECK(commandResult(controller,
                                controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
            const auto undone = "after undoing " + choice.name.toStdString();
            expectBands(fixture, undone, "stage2", left);
            expectBands(fixture, undone, "stage3", right);
            CHECK(commandResult(controller,
                                controller.commands()->redo("choices", QString::number(fixture.file().current().id))));
            expectBands(fixture, "after redo", "stage2", choice.left);
            expectBands(fixture, "after redo", "stage3", choice.right);
            CHECK(commandResult(controller,
                                controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
            expectBands(fixture, "after undoing redo", "stage2", left);
            expectBands(fixture, "after undoing redo", "stage3", right);
        }
    });
    scenes.run("hover raises counterpart backgrounds and connector bands", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        settlePanes(fixture);
        const auto bands = paintedBands(fixture, "stage2");
        CHECK(!bands.empty());
        const auto band = bands.front();
        auto *source = fixture.item("stage2Viewport");
        auto *editor = fixture.item("resultText");
        select(fixture, 0, 3);
        const auto selectionStart = editor->property("selectionStart");
        const auto selectionEnd = editor->property("selectionEnd");
        const auto cursor = editor->property("cursorPosition");
        const auto before = fixture.file().current();
        const auto point =
            source
                ->mapToScene(QPointF(source->width() / 2,
                                     (band.sourceTop + band.sourceBottom) / 2 - source->mapToScene(QPointF()).y()))
                .toPoint();
        const auto previous = fixture.item("stage2Backgrounds")->property("current");
        QTest::mouseMove(fixture.window, point);
        const auto highlighted = [&](const char *name, int line) {
            auto *layer = fixture.item(name);
            const auto color = qmlValue(layer, "Kirigami.Theme.highlightColor").value<QColor>();
            for (auto *fill : fixture.item((std::string(name) + "Current").c_str())->childItems()) {
                if (fill->isVisible() && fill->property("line").toInt() == line
                    && fill->property("color").value<QColor>() == color)
                    return true;
            }
            return false;
        };
        settleFor("hover counterpart", [&] {
            return highlighted("stage2Backgrounds", band.sourceBegin)
                && highlighted("resultBackgrounds", band.resultBegin);
        });
        bool raisedBand = false;
        for (auto *item : fixture.item("stage2Bands")->childItems()) raisedBand |= item->property("current").toBool();
        CHECK(raisedBand);
        CHECK(editor->property("selectionStart") == selectionStart);
        CHECK(editor->property("selectionEnd") == selectionEnd);
        CHECK(editor->property("cursorPosition") == cursor);
        CHECK(fixture.file().current().id == before.id);
        CHECK(fixture.file().current().draft == before.draft);
        QTest::mouseMove(fixture.window, QPoint(1, 1));
        settleFor("hover clears", [&] { return fixture.item("stage2Backgrounds")->property("current") == previous; });
    });
    scenes.run("band ends move with the scroll of either pane", [] {
        // The result holds 30 lines after line 50 that the left source lacks, so the
        // left handle draws one band, a wedge on the left source's line boundary.
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, insertionFile());
        auto *left = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *editor = fixture.item("resultText");
        settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
        settlePanes(fixture);
        const auto line = paneLineHeight(editor);
        wheelTo(fixture, "stage2Viewport", 40 * line);
        const std::vector<std::string> band{"added 51-51>51-81"};
        expectBands(fixture, "at the insertion", "stage2", band);
        CHECK(bandLines(fixture, "stage3").empty());
        const auto before = paintedBands(fixture, "stage2").front();
        // Scrolling one pane without leading the others moves only that pane's end.
        left->setProperty("contentY", contentY(left) + 3 * line);
        expectBands(fixture, "with the left source scrolled", "stage2", band);
        const auto moved = paintedBands(fixture, "stage2").front();
        CHECK(std::abs(moved.sourceTop - (before.sourceTop - 3 * line)) < 1);
        CHECK(std::abs(moved.resultTop - before.resultTop) < 0.5);
        result->setProperty("contentY", contentY(result) + 2 * line);
        expectBands(fixture, "with the result scrolled", "stage2", band);
        const auto after = paintedBands(fixture, "stage2").front();
        CHECK(std::abs(after.sourceTop - moved.sourceTop) < 0.5);
        CHECK(std::abs(after.resultTop - (moved.resultTop - 2 * line)) < 1);
    });
    scenes.run("the band handles drag to resize the panes and stay out of the focus chain", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller);
        settlePanes(fixture);
        auto *handle = fixture.item("stage2Connector");
        auto *left = fixture.item("stage2Viewport");
        auto *result = fixture.item("resultViewport");
        auto *other = fixture.item("stage3Connector");
        CHECK(handle != other && handle->width() > 0 && !handle->activeFocusOnTab());
        // Tabbing through the window reaches neither handle nor any of its bands.
        auto *first = fixture.window->contentItem()->nextItemInFocusChain();
        auto *item = first;
        for (int step = 0; item && step < 1000; ++step) {
            CHECK(item != handle && item != other && !handle->isAncestorOf(item) && !other->isAncestorOf(item));
            item = item->nextItemInFocusChain();
            if (item == first) break;
        }
        CHECK(item == first);
        // Dragging the handle widens the left source at the expense of the right
        // source, which fills the split, and leaves the result as it was.
        auto *right = fixture.item("stage3Viewport");
        const auto bandsBefore = bandLines(fixture, "stage2");
        CHECK(bandsBefore == (std::vector<std::string>{"changed 2-3>2-3", "conflict 6-7>6-7"}));
        const auto leftWidth = left->width();
        const auto resultWidth = result->width();
        const auto rightWidth = right->width();
        const auto from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
        QTest::mousePress(fixture.window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 10; ++step) QTest::mouseMove(fixture.window, QPoint(from.x() + 8 * step, from.y()));
        QTest::mouseRelease(fixture.window, Qt::LeftButton, Qt::NoModifier, QPoint(from.x() + 80, from.y()));
        settle([&] { return std::abs(left->width() - (leftWidth + 80)) < 2; });
        CHECK(std::abs(result->width() - resultWidth) < 1 && std::abs(right->width() - (rightWidth - 80)) < 2);
        expectBands(fixture, "after the drag", "stage2", bandsBefore);
    });
}
