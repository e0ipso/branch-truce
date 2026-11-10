// How the panes show text: search, wrapping, line numbers, tab stops, fonts and whitespace markers.
#include "ComparisonScene.h"

using namespace comparison_scene;

void comparison_scene::addTextCases(Groups groups)
{
    auto &scenes = groups.scenes;
    scenes.run("search scope switches the pane Find selects", [] {
        Fixture fixture;
        fixture.click("openPassageSearch");
        const auto before = captureWork(fixture);
        auto *scope = fixture.item("searchScope");
        auto *query = fixture.item("searchQuery");
        CHECK(scope->property("count").toInt() == 3);
        CHECK(scope->property("currentIndex").toInt() == 0);
        CHECK(scope->property("currentText").toString() == "Left source");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Search scope: Left source."));
        CHECK(query->setProperty("text", "left only"));
        fixture.click("findPassage");
        auto *left = fixture.item("stage2Text");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Passage in Left source."));
        CHECK(left->property("selectedText").toString() == "left only");
        CHECK(left->property("selectionStart").toInt() == left->property("text").toString().indexOf("left only"));
        CHECK(fixture.item("resultText")->property("selectedText").toString().isEmpty());
        CHECK(fixture.item("stage3Text")->property("selectedText").toString().isEmpty());
        checkUnchanged(fixture, before);

        CHECK(scope->setProperty("currentIndex", 2));
        CHECK(scope->property("currentText").toString() == "Right source");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Search scope: Right source."));
        fixture.click("findPassage");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("No passage in Right source."));
        CHECK(fixture.item("stage3Text")->property("selectedText").toString().isEmpty());
        CHECK(query->setProperty("text", "right only"));
        fixture.click("findPassage");
        auto *right = fixture.item("stage3Text");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Passage in Right source."));
        CHECK(right->property("selectedText").toString() == "right only");
        CHECK(right->property("selectionStart").toInt() == right->property("text").toString().indexOf("right only"));
        checkUnchanged(fixture, before);

        CHECK(scope->setProperty("currentIndex", 1));
        CHECK(scope->property("currentText").toString() == "Editable result");
        CHECK(query->setProperty("text", "base conflict"));
        fixture.click("findPassage");
        auto *result = fixture.item("resultText");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Passage in Editable result."));
        CHECK(result->property("selectedText").toString() == "base conflict");
        CHECK(result->property("selectionStart").toInt()
              == result->property("text").toString().indexOf("base conflict"));
        checkUnchanged(fixture, before);

        CHECK(fixture.item("showEarlier")->setProperty("checked", true));
        settle([&] { return fixture.item("earlierText")->isVisible() && scope->property("count").toInt() == 4; });
        CHECK(scope->property("currentText").toString() == "Editable result");
        CHECK(scope->setProperty("currentIndex", 3));
        CHECK(scope->property("currentText").toString() == "Earlier version");
        fixture.click("findPassage");
        auto *earlier = fixture.item("earlierText");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Passage in Earlier version."));
        CHECK(earlier->property("selectedText").toString() == "base conflict");
        CHECK(earlier->property("selectionStart").toInt()
              == earlier->property("text").toString().indexOf("base conflict"));
        checkUnchanged(fixture, before);
    });
    scenes.run("Find selects a passage and wraps to the earlier match", [] {
        const std::string result = "prefix REPEAT middle REPEAT suffix\n";
        Fixture fixture(test_support::choiceFile("left\n", "right\n", result));
        fixture.click("openPassageSearch");
        const auto before = captureWork(fixture);
        auto *scope = fixture.item("searchScope");
        CHECK(scope->setProperty("currentIndex", 1));
        CHECK(scope->property("currentText").toString() == "Editable result");
        CHECK(fixture.item("searchQuery")->setProperty("text", "REPEAT"));
        auto *editor = fixture.item("resultText");
        const auto text = editor->property("text").toString();
        const auto first = text.indexOf("REPEAT");
        const auto second = text.indexOf("REPEAT", first + 6);
        CHECK(first >= 0 && second > first);
        fixture.click("findPassage");
        CHECK(editor->property("selectedText").toString() == "REPEAT");
        CHECK(editor->property("selectionStart").toInt() == first);
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Passage in Editable result."));
        checkUnchanged(fixture, before);
        fixture.click("findPassage");
        CHECK(editor->property("selectionStart").toInt() == second);
        CHECK(editor->property("selectedText").toString() == "REPEAT");
        checkUnchanged(fixture, before);
        fixture.click("findPassage");
        CHECK(editor->property("selectionStart").toInt() == first);
        CHECK(editor->property("selectedText").toString() == "REPEAT");
        CHECK(fixture.item("searchStatus")->property("text").toString().startsWith("Passage in Editable result."));
        checkUnchanged(fixture, before);
    });
    scenes.run("wrapping a long line changes its visual line count", [] {
        const std::string result(480, 'M');
        Fixture fixture(test_support::choiceFile("left\n", "right\n", result + "\n"));
        const auto before = captureWork(fixture);
        auto *editor = fixture.item("resultText");
        auto *wrap = fixture.item("wrapLines");
        CHECK(!wrap->property("checked").toBool());
        const auto unwrappedMode = editor->property("wrapMode").toInt();
        settle([&] { return visualLines(editor, 0) == 1; });
        const auto unwrappedHeight = editor->property("contentHeight").toReal();
        CHECK(unwrappedHeight > 0);
        CHECK(wrap->setProperty("checked", true));
        settle([&] { return visualLines(editor, 0) > 1; });
        CHECK(editor->property("wrapMode").toInt() != unwrappedMode);
        CHECK(editor->property("contentHeight").toReal() > unwrappedHeight);
        checkUnchanged(fixture, before);
        CHECK(wrap->setProperty("checked", false));
        settle([&] { return visualLines(editor, 0) == 1; });
        CHECK(editor->property("wrapMode").toInt() == unwrappedMode);
        checkUnchanged(fixture, before);
    });
    scenes.run("each line number stays on the first row of its line while wrapped and scrolled", [] {
        // Every seventh line is long enough to wrap onto several rows.
        std::string result;
        for (int line = 1; line <= 300; ++line)
            result += line % 7 == 0 ? std::string(400, 'w') + "\n" : "line " + std::to_string(line) + "\n";
        Fixture fixture(test_support::choiceFile("left\n", result, result));
        auto *editor = fixture.item("resultText");
        auto *viewport = fixture.item("resultViewport");
        settle([&] { return editor->property("lineCount").toInt() >= 300; });
        settlePanes(fixture);
        const auto aligned = [&](const char *what) {
            for (const std::string pane : {"result", "stage3"}) {
                settleFor(pane + " line numbers " + what, [&] { return gutterMisalignment(fixture, pane).empty(); });
                CHECK(gutterMisalignment(fixture, pane).empty());
            }
        };
        aligned("unwrapped");
        CHECK(fixture.item("wrapLines")->setProperty("checked", true));
        settle([&] { return visualLines(editor, 6) > 1; });
        aligned("wrapped");
        // Halfway down, a wrapped line is in view and the numbers follow the scroll.
        const auto half = (viewport->property("contentHeight").toReal() - viewport->height()) / 2;
        CHECK(viewport->setProperty("contentY", half));
        aligned("wrapped and scrolled");
        const auto top = viewport->mapToScene(QPointF()).y();
        bool wrappedInView = false;
        const auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            const auto row = sceneRectangle(editor, block.position());
            wrappedInView = wrappedInView
                || (row.top() >= top && row.top() < top + viewport->height()
                    && visualLines(editor, block.blockNumber()) > 1);
        }
        CHECK(wrappedInView);
        CHECK(fixture.item("wrapLines")->setProperty("checked", false));
        settle([&] { return visualLines(editor, 6) == 1; });
        aligned("unwrapped again");
    });
    scenes.run("tabs stop every four characters at the same column in every pane", [] {
        const std::string shared = "abcde\n\tone\nabc\ttwo\nabcde\tthree\n\t\tfour\n";
        Fixture fixture(test_support::choiceFile(shared + "left\n", shared + "right\n", shared + "right\n"));
        settlePanes(fixture);
        // The text after the tabs, its column in the line, and the tab stops it sits on.
        const std::array<std::tuple<QString, int, int>, 4> lines{
            {{"\tone", 1, 1}, {"abc\ttwo", 4, 1}, {"abcde\tthree", 6, 2}, {"\t\tfour", 2, 2}}};
        const auto checkTabs = [&] {
            // Four characters as drawn in the result, independent of font metrics metadata.
            const auto four = columnOffset(fixture.item("resultText"), QStringLiteral("abcde\n"), 4);
            CHECK(four > 0);
            for (const auto *name : {"stage2Text", "resultText", "stage3Text"}) {
                auto *editor = fixture.item(name);
                CHECK(std::abs(columnOffset(editor, QStringLiteral("abcde\n"), 4) - four) < 0.01);
                for (const auto &[text, column, stops] : lines) {
                    const auto offset = columnOffset(editor, text, column);
                    if (std::abs(offset - stops * four) > 0.5)
                        throw std::runtime_error(std::string(name) + ": text after the tab in " + text.toStdString()
                                                 + " starts at " + std::to_string(offset) + ", not "
                                                 + std::to_string(stops * four));
                }
            }
        };
        checkTabs();
        // Theme fonts may arrive after creation or change while the window is open.
        auto larger = fixture.item("resultText")->property("font").value<QFont>();
        larger.setPointSizeF(larger.pointSizeF() + 3);
        const auto smaller = columnOffset(fixture.item("resultText"), QStringLiteral("abcde\n"), 4);
        for (const auto *name : {"stage2Text", "resultText", "stage3Text"})
            CHECK(fixture.item(name)->setProperty("font", larger));
        // Measure only once every pane has laid out its text in the larger font.
        test_support::waitUntil(
            [&] {
                for (const auto *name : {"stage2Text", "resultText", "stage3Text"})
                    if (columnOffset(fixture.item(name), QStringLiteral("abcde\n"), 4) <= smaller + 0.5) return false;
                return true;
            },
            "the panes did not lay out the larger font");
        checkTabs();
    });
    scenes.run("code and line numbers use the theme's fixed-width font", [] {
        auto sources = *test_support::choiceFile().snapshots();
        const auto earlier = sources.stage2->worktree.utf8;
        sources.earlier = SourceSnapshot{"earlier-object", "100644",
                                         earlier,          test_support::choiceText(earlier),
                                         "earlier-commit", "Common earlier version"};
        const auto launch = sources.launchResult.text.utf8;
        Fixture fixture(FileSession("choices", sources.path, {launch, {}, 0}, Support::Supported, {}, sources, true));
        fixture.click("showEarlier");
        settle([&] { return fixture.item("earlierText")->isVisible(); });
        settlePanes(fixture);
        for (const std::string pane : {"stage2", "result", "stage3", "earlier"}) {
            auto *editor = fixture.item((pane + "Text").c_str());
            const auto fixed = qmlValue(editor, QStringLiteral("Kirigami.Theme.fixedWidthFont")).value<QFont>();
            const auto font = editor->property("font").value<QFont>();
            if (font != fixed)
                throw std::runtime_error(pane + " uses " + font.toString().toStdString() + ", not the fixed-width "
                                         + fixed.toString().toStdString());
            auto *gutter = fixture.item((pane + "Gutter").c_str());
            settleFor(pane + " line numbers", [&] { return !gutterNumbers(gutter).isEmpty(); });
            for (auto *child : gutter->childItems()) {
                bool number = false;
                child->property("text").toString().toInt(&number);
                if (number) CHECK(child->property("font").value<QFont>() == fixed);
            }
        }
    });
    scenes.run("whitespace markers paint spaces and tabs", [] {
        const std::string result = "aa bb\tcc\n";
        Fixture fixture(test_support::choiceFile("left\n", "right\n", result));
        const auto before = captureWork(fixture);
        auto *markers = fixture.item("resultWhitespace");
        auto *toggle = fixture.item("showWhitespace");
        CHECK(!toggle->property("checked").toBool());
        settle([&] { return paintedMarkers(markers) == 0; });
        CHECK(markerKind(markers, false) == 0 && markerKind(markers, true) == 0);
        CHECK(toggle->setProperty("checked", true));
        settle([&] { return paintedMarkers(markers) == 2; });
        CHECK(markerKind(markers, false) == 1 && markerKind(markers, true) == 1);
        CHECK(paintedMarkers(fixture.item("stage2Whitespace")) == 0);
        CHECK(paintedMarkers(fixture.item("stage3Whitespace")) == 0);
        CHECK(fixture.item("resultText")->property("text").toString().toStdString() == result);
        checkUnchanged(fixture, before);
        CHECK(toggle->setProperty("checked", false));
        settle([&] { return paintedMarkers(markers) == 0; });
        CHECK(fixture.item("resultText")->property("text").toString().toStdString() == result);
        checkUnchanged(fixture, before);
    });
}
