// Conflict markers in the result: folded blocks, their rows and choices, and conflict navigation.
#include "ComparisonCases.h"
#include "ComparisonEditor.h"
#include "ComparisonFolds.h"
#include "ComparisonGeometry.h"
#include "ComparisonMarks.h"
#include "ComparisonNavigationChecks.h"
#include "ComparisonRecovery.h"
#include "ComparisonSessionChecks.h"
#include "ComparisonTextProbes.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"

using test_support::await;
using test_support::commandResult;

using namespace comparison_scene;

void comparison_scene::addFoldingCases(Groups groups)
{
    auto &scenes = groups.scenes;
    scenes.run(
        "each marker block folds into one labelled row behind a conflict band, keeps the buffer and shows on the "
        "toggle",
        [] {
            // markedFile(true) writes both sections between markers with diff3 base sections:
            // blocks on lines 2-11 and 13-19, whose marker lines are 2, 5, 8, 11, 13, 15, 17 and 19.
            Application controller(test_support::windowConfig());
            Fixture fixture(controller, test_support::markedFile(true));
            settlePanes(fixture);
            const auto text = QString::fromStdString(test_support::markedText(true));
            const auto markerLines = linesStarting(fixture, {"<<<<<<<", "|||||||", "=======", ">>>>>>>"});
            CHECK(markerLines == QList<int>({2, 5, 8, 11, 13, 15, 17, 19}));
            const auto lineCount = resultDocument(fixture).blockCount();
            const auto blockLines = numbers(2, 12) + numbers(13, 20);
            // Folded: each block keeps one row on its opening line, which keeps its number and is
            // marked as a conflict with markers remaining; the rest of the block takes no row and
            // gets no number, so the numbers go from 2 to 12 and from 13 to 20.
            const auto hidden = numbers(3, 12) + numbers(14, 20);
            const MarkerLook folded{hidden, numbers(1, lineCount + 1, hidden), {2, 13}, {2, 13}, 2};
            expectLook(fixture, "at launch", folded);
            CHECK(fixture.text() == text && fixture.file().current().draft.text == text.toStdString());
            CHECK(resultDocument(fixture).toPlainText() == text);
            // The row has the conflict colours: the ! glyph and the label in negativeTextColor,
            // the label DemiBold, on negativeBackgroundColor. Each layer's own theme gives the
            // colours, as the backgrounds layer takes no input.
            const auto theme = [&](const char *layer, const char *role) {
                return qmlValue(fixture.item(layer), QStringLiteral("Kirigami.Theme.") + QLatin1String(role))
                    .value<QColor>();
            };
            for (auto *fill : fixture.item("resultBackgroundsLines")->childItems()) {
                if (fill->isVisible() && fill->property("kind").toString() == "markers")
                    CHECK(fill->property("color").value<QColor>()
                          == theme("resultBackgrounds", "negativeBackgroundColor"));
            }
            for (auto *glyph : fixture.item("resultGutterMarkerSlot")->childItems()) {
                if (glyph->isVisible() && glyph->property("kind").toString() == "markers") {
                    CHECK(glyph->property("text").toString() == "!");
                    CHECK(glyph->property("color").value<QColor>()
                          == theme("resultGutterMarkerSlot", "negativeTextColor"));
                }
            }
            for (auto *row : foldParts(fixture.item("resultFoldedRows"))) {
                auto *fill = row->property("item").value<QQuickItem *>();
                CHECK(fill
                      && fill->property("color").value<QColor>()
                          == theme("resultFoldedRows", "negativeBackgroundColor"));
                // The row covers its whole line, from the gutter to the right edge.
                CHECK(row->width() >= fixture.item("resultViewport")->width() && row->height() > 0);
            }
            auto *label = fixture.item("resultConflictRow-2");
            CHECK(label->isVisible() && label->property("text").toString() == "Unresolved conflict");
            CHECK(label->property("color").value<QColor>() == theme("resultFoldedRows", "negativeTextColor"));
            CHECK(label->property("font").value<QFont>().weight() == QFont::DemiBold);
            const auto described =
                qmlValue(foldParts(fixture.item("resultFoldedRows")).value(2)->property("item").value<QQuickItem *>(),
                         QStringLiteral("Accessible.description"))
                    .toString();
            if (described != "Lines 2 to 11 are folded. Show conflict markers shows them.")
                throw std::runtime_error("first row described as " + described.toStdString());
            // Each row offers Take both for its section, beside the label.
            for (const std::size_t at : {std::size_t{0}, std::size_t{1}}) {
                const auto &target =
                    test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section, at);
                const auto name = "resultTakeBoth-" + target.source.contributionId;
                auto *takeBoth = fixture.item(name.c_str());
                CHECK(takeBoth->isVisible() && takeBoth->isEnabled()
                      && takeBoth->property("text").toString() == "Take both");
                CHECK(qmlValue(takeBoth, QStringLiteral("icon.name")).toString() == "merge");
                CHECK(!takeBoth->property("flat").toBool());
                CHECK(takeBoth->activeFocusOnTab());
            }
            // Every band to the result meets a block, so each side has one conflict band per
            // block, from its lines to the whole block, which ends on the block's row.
            for (const auto *side : {"stage2", "stage3"}) {
                const auto bands = bandLines(fixture, side);
                CHECK(bands.size() == 2);
                for (const auto &band : bands)
                    if (band.rfind("conflict ", 0) != 0) throw std::runtime_error(std::string(side) + " band " + band);
                CHECK(bands[0].ends_with(">2-12") && bands[1].ends_with(">13-20"));
            }
            // The toggle shows the raw lines as conflicts, drops the rows and changes no work.
            const auto before = captureWork(fixture, *controller.selectedFile());
            auto *toggle = fixture.object("showConflictMarkers");
            CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
            CHECK(toggle->property("checked").toBool());
            expectLook(fixture, "with the markers shown", {{}, numbers(1, lineCount + 1), blockLines, {}, 11});
            checkUnchanged(fixture, *controller.selectedFile(), before);
            CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
            expectLook(fixture, "with the markers folded again", folded);
            checkUnchanged(fixture, *controller.selectedFile(), before);
        });
    scenes.run("unmapped recognized markers fold without offering unsafe choices", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::underlinedBaseFile(false));
        settlePanes(fixture);
        CHECK(fixture.file().markers().blocks.size() == 1 && fixture.file().markerSections().empty());
        const auto blocks = controller.selectedFile()->comparison()->markerBlocks();
        CHECK(blocks.size() == 1 && blocks.front().toMap().value("folded").toBool());
        CHECK(blocks.front().toMap().value("sections").toStringList().isEmpty());
        settle([&] { return foldedLines(fixture) == numbers(3, 15); });
        CHECK(foldParts(fixture.item("resultFoldedRows")).keys() == QList<int>({2}));
        CHECK(fixture.item("resultConflictRow-2")->property("text").toString() == "Unresolved conflict");
        for (auto *item : fixture.item("resultFoldedRows")->findChildren<QQuickItem *>())
            CHECK(!item->objectName().startsWith("resultTakeBoth-") || !item->isVisible());
        const auto original = captureWork(fixture, *controller.selectedFile());
        const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        CHECK(!test_support::previewResult(
            controller,
            controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                QString::fromStdString(target.source.contributionId),
                                                ChoicePreviewFlow::Action::CombineLeftFirst)));
        checkUnchanged(fixture, *controller.selectedFile(), original);
        auto *toggle = fixture.object("showConflictMarkers");
        CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
        settle([&] { return foldedLines(fixture).isEmpty(); });
        checkUnchanged(fixture, *controller.selectedFile(), original);
        // Edit the recognizable closing marker while raw lines are visible.
        const auto end = static_cast<int>(fixture.text().indexOf("theirs") + 6);
        select(fixture, end, end);
        key(fixture, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
        const auto edited = captureWork(fixture, *controller.selectedFile());
        CHECK(edited.draft.text != original.draft.text && edited.draft.decisions == original.draft.decisions);
        CHECK(fixture.file().markers().blocks.size() == 1 && fixture.file().markerSections().empty());
        CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
        settle([&] { return foldedLines(fixture) == numbers(3, 15); });
        checkUnchanged(fixture, *controller.selectedFile(), edited);
        CHECK(commandResult(controller,
                            controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().current().draft == original.draft);
        settle([&] { return foldedLines(fixture) == numbers(3, 15); });
        CHECK(commandResult(controller,
                            controller.commands()->redo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().current().draft == edited.draft);
        settle([&] { return foldedLines(fixture) == numbers(3, 15); });
        const auto redone = captureWork(fixture, *controller.selectedFile());
        CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
        settle([&] { return foldedLines(fixture).isEmpty(); });
        checkUnchanged(fixture, *controller.selectedFile(), redone);
        checkRecovered(*controller.store(), *controller.files(), edited.draft);
    });
    scenes.run("a long marker label stays one row with wrapping enabled", [] {
        auto marked = test_support::markedText();
        marked.replace(marked.find("<<<<<<< ours"), std::string("<<<<<<< ours").size(),
                       "<<<<<<< " + std::string(400, 'x'));
        Application controller(test_support::windowConfig());
        Fixture fixture(controller,
                        test_support::choiceFile("header\nleft first\nleft second\nbridge\nleft later\ntail\n",
                                                 "header\nright first\nright second\nbridge\nright later\ntail\n",
                                                 marked));
        settlePanes(fixture);
        const auto before = captureWork(fixture, *controller.selectedFile());
        CHECK(fixture.item("wrapLines")->setProperty("checked", true));
        expectLook(fixture, "folded with wrapping", markedFolded());
        auto &document = resultDocument(fixture);
        settle([&] { return document.findBlockByNumber(1).layout()->lineCount() == 1; });
        CHECK(QMetaObject::invokeMethod(fixture.object("showConflictMarkers"), "trigger"));
        settle([&] { return document.findBlockByNumber(1).layout()->lineCount() > 1; });
        CHECK(QMetaObject::invokeMethod(fixture.object("showConflictMarkers"), "trigger"));
        expectLook(fixture, "folded again with wrapping", markedFolded());
        checkUnchanged(fixture, *controller.selectedFile(), before);
    });
    scenes.run("keys and selections step over a folded block and its row", [] {
        // markedFile(): the first block is lines 2-8, folded into its row on line 2.
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::markedFile());
        settlePanes(fixture);
        expectLook(fixture, "at launch", markedFolded());
        const auto text = fixture.text();
        const auto before = captureWork(fixture, *controller.selectedFile());
        const auto at = [&](const char *line) { return static_cast<int>(text.indexOf(QLatin1String(line))); };
        auto *editor = fixture.item("resultText");
        const auto selection = [&] {
            return std::pair{editor->property("selectionStart").toInt(), editor->property("selectionEnd").toInt()};
        };
        // Down from column 3 of the header passes the row and the folded block and keeps the column.
        select(fixture, 3, 3);
        expectCursor(fixture, "after placing the cursor", 3);
        key(fixture, Qt::Key_Down);
        expectCursor(fixture, "down from the header", at("bridge") + 3);
        key(fixture, Qt::Key_Up);
        expectCursor(fixture, "up from bridge", 3);
        // Right at the end of the header and Left at the start of bridge cross the block.
        select(fixture, at("header") + 6, at("header") + 6);
        key(fixture, Qt::Key_Right);
        expectCursor(fixture, "right over the block", at("bridge"));
        key(fixture, Qt::Key_Left);
        expectCursor(fixture, "left over the block", at("header") + 6);
        // Shift+Down extends a selection over the block and keeps its anchor.
        select(fixture, 0, 0);
        key(fixture, Qt::Key_Down, Qt::ShiftModifier);
        expectCursor(fixture, "shift+down over the block", at("bridge"));
        CHECK(selection() == std::pair(0, at("bridge")));
        // Consecutive keys can arrive before the queued cursor check. Each uses the
        // preceding visible position and keeps the original selection anchor.
        select(fixture, 3, 3);
        key(fixture, Qt::Key_Down, Qt::ShiftModifier);
        key(fixture, Qt::Key_Down, Qt::ShiftModifier);
        expectCursor(fixture, "two shift+down keys", at("tail") + 3);
        CHECK(selection() == std::pair(3, at("tail") + 3));
        key(fixture, Qt::Key_Up, Qt::ShiftModifier);
        expectCursor(fixture, "shift+up from tail", at("bridge") + 3);
        CHECK(selection() == std::pair(3, at("bridge") + 3));
        // A selection set by code that ends in a folded block ends before its row instead.
        select(fixture, 0, at("======="));
        expectCursor(fixture, "after selecting into the block", at("header") + 6);
        CHECK(selection() == std::pair(0, at("header") + 6));
        // A cursor set by code on the row goes on to the line after the block.
        select(fixture, at("<<<<<<< ours") + 2, at("<<<<<<< ours") + 2);
        expectCursor(fixture, "after placing the cursor on the row", at("bridge"));
        CHECK(fixture.text() == text);
        expectLook(fixture, "after keyboard navigation", markedFolded());
        checkUnchanged(fixture, *controller.selectedFile(), before);
    });
    scenes.run("a recognizable edited end marker remains folded and can be shown", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::markedFile());
        settlePanes(fixture);
        const auto folded = markedFolded();
        expectLook(fixture, "at launch", folded);
        const auto launched = fixture.text();
        // Backspace at the start of "bridge" joins it to the folded end marker above it.
        const auto bridge = static_cast<int>(launched.indexOf("bridge"));
        select(fixture, bridge, bridge);
        key(fixture, Qt::Key_Backspace);
        const auto joined = QString(launched).replace(">>>>>>> theirs\nbridge", ">>>>>>> theirsbridge");
        CHECK(fixture.text() == joined && fixture.file().current().draft.text == joined.toStdString());
        // The edited end marker is still recognized, but no longer maps to a choice.
        const QList<int> hidden = numbers(3, 9) + numbers(10, 14);
        const MarkerLook joinedFold{hidden, numbers(1, 16, hidden), {2, 9}, {2, 9}, 0};
        expectLook(fixture, "after joining a line to the end marker", joinedFold);
        CHECK(fixture.file().markers().blocks.size() == 2 && fixture.file().markerSections().size() == 1);
        checkRecovered(*controller.store(), *controller.files(), fixture.file().current().draft);
        const auto afterJoin = captureWork(fixture, *controller.selectedFile());
        auto *toggle = fixture.object("showConflictMarkers");
        CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
        settle([&] { return foldedLines(fixture).isEmpty(); });
        select(fixture, bridge - 1, bridge - 1);
        expectCursor(fixture, "on the shown edited marker", bridge - 1);
        checkUnchanged(fixture, *controller.selectedFile(), afterJoin);
        CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
        expectLook(fixture, "after folding the edited marker again", joinedFold);
        checkUnchanged(fixture, *controller.selectedFile(), afterJoin);
        // Undo and redo restore the bytes and their recognized folds.
        CHECK(commandResult(controller,
                            controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.text() == launched);
        expectLook(fixture, "after undo", folded);
        CHECK(commandResult(controller,
                            controller.commands()->redo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.text() == joined);
        expectLook(fixture, "after redo", joinedFold);
        checkRecovered(*controller.store(), *controller.files(), fixture.file().current().draft);
    });
    scenes.run("finding text on a folded line or going to a blocking marker shows the markers", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::markedFile());
        click(fixture, "openPassageSearch");
        settlePanes(fixture);
        CHECK(foldedLines(fixture) == markedFolded().hidden);
        // Find in the result selects a match on a folded end marker and shows the markers.
        auto *toggle = fixture.object("showConflictMarkers");
        CHECK(fixture.item("searchScope")->setProperty("currentIndex", 1));
        CHECK(fixture.item("searchQuery")->setProperty("text", QStringLiteral("theirs")));
        click(fixture, "findPassage");
        const auto theirs = static_cast<int>(fixture.text().indexOf("theirs"));
        expectCursor(fixture, "after finding theirs", theirs + 6);
        CHECK(fixture.item("resultText")->property("selectionStart").toInt() == theirs);
        CHECK(toggle->property("checked").toBool() && foldedLines(fixture).isEmpty());
        CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
        settle([&] { return foldedLines(fixture) == markedFolded().hidden; });
        CHECK(!controller.commands()->markResolved("choices", QString::number(fixture.file().current().id), true));
        CHECK(controller.feedback()->markerBlocker().value("line").toInt() == 2);
        const auto before = captureWork(fixture, *controller.selectedFile());
        CHECK(QMetaObject::invokeMethod(fixture.object("showFirstMarker"), "trigger"));
        // The cursor stays on the marker it went to, the row's line, which the view now shows.
        expectCursor(fixture, "after going to the first marker",
                     static_cast<int>(fixture.text().indexOf("<<<<<<< ours")));
        CHECK(toggle->property("checked").toBool() && foldedLines(fixture).isEmpty());
        checkUnchanged(fixture, *controller.selectedFile(), before);
    });
    scenes.run("deciding a section removes its fold and markersRemain, and Save writes the buffer either way", [] {
        const auto remaining = [](const SelectedFile &selected) {
            QStringList found;
            for (const auto &entry : selected.comparison()->contributions()) {
                const auto &region = entry;
                found << region.side + ":" + region.section + "=" + (region.markersRemain ? "remain" : "gone");
            }
            found.sort();
            return found;
        };
        // Save with the markers folded and with them shown writes the same bytes, the buffer's.
        std::optional<std::string> savedBytes;
        for (const bool shown : {false, true}) {
            Application controller(test_support::windowConfig());
            Fixture fixture(controller, test_support::markedFile());
            settlePanes(fixture);
            if (shown) CHECK(QMetaObject::invokeMethod(fixture.object("showConflictMarkers"), "trigger"));
            expectLook(fixture, shown ? "shown before Save" : "folded before Save",
                       shown ? MarkerLook{{}, numbers(1, 17), numbers(2, 9) + numbers(10, 15), {}, 8} : markedFolded());
            const auto header = static_cast<int>(fixture.text().indexOf("header"));
            select(fixture, header, header);
            key(fixture, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            settle([&] { return controller.selectedFile()->unsaved(); });
            click(fixture, "saveDraft");
            CHECK(fixture.file().publication().checkpoint());
            const auto replay = replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.size() == 1 && replay.files.front().publication().checkpoint());
            const auto bytes =
                replay.files.front().revision(*replay.files.front().publication().checkpoint())->draft.text;
            CHECK(bytes == "x" + test_support::markedText()
                  && bytes == fixture.file().revision(*fixture.file().publication().checkpoint())->draft.text);
            CHECK(QString::fromStdString(bytes) == fixture.text());
            if (savedBytes) CHECK(bytes == *savedBytes);
            savedBytes = bytes;
        }
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::markedFile());
        settlePanes(fixture);
        const auto atLaunch = remaining(*controller.selectedFile());
        CHECK(atLaunch.size() >= 4 && atLaunch.filter("=gone").isEmpty());
        CHECK(controller.selectedFile()->comparison()->markerBlocks().size() == 2);
        // Resolving the first section with the left side removes its markers, so only the
        // second block stays folded and only its regions still have markers.
        const auto &first = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        const auto section = QString::fromStdString(first.section);
        CHECK(test_support::previewResult(
            controller,
            controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                QString::fromStdString(first.source.contributionId),
                                                ChoicePreviewFlow::Action::ResolveUsingSide)));
        CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
        CHECK(!fixture.text().contains(QStringLiteral(">>>>>>> theirs\nbridge")));
        CHECK(controller.selectedFile()->comparison()->markerBlocks().size() == 1);
        for (const auto &entry : remaining(*controller.selectedFile()))
            CHECK(entry.endsWith(entry.contains(":" + section + "=") ? "=gone" : "=remain"));
        // The second block, now lines 5-9, folds into its row on line 5.
        expectLook(fixture, "after resolving the first section",
                   {{6, 7, 8, 9}, numbers(1, 12, {6, 7, 8, 9}), {5}, {5}, 3});
        CHECK(commandResult(controller,
                            controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(remaining(*controller.selectedFile()) == atLaunch);
        expectLook(fixture, "after undoing the choice", markedFolded());
    });
    scenes.run("settling every choice shows the ready notice and row until undo reopens one", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::markedFile());
        settlePanes(fixture);
        auto *notice = fixture.item("settledNotice");
        const auto rowLine = [&] {
            const auto row = controller.files()->rowOf("choices");
            return controller.files()->data(controller.files()->index(row), SessionModel::StatusLineRole).toString();
        };
        CHECK(!notice->isVisible() && rowLine() != QStringLiteral("Ready to mark resolved"));
        const auto resolve = [&](SourceSide side, std::size_t section) {
            const auto &target = test_support::choiceTarget(fixture.file(), side, ChoiceScope::Section, section);
            CHECK(test_support::previewResult(
                controller,
                controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                    QString::fromStdString(target.source.contributionId),
                                                    ChoicePreviewFlow::Action::ResolveUsingSide)));
            CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
        };
        resolve(SourceSide::Stage2, 0);
        CHECK(!notice->isVisible());
        resolve(SourceSide::Stage3, 1);
        settle([&] { return notice->isVisible(); });
        CHECK(notice->isVisible() && rowLine() == QStringLiteral("Ready to mark resolved"));
        // Nothing was written or resolved.
        CHECK(!fixture.file().facts().resolved && !fixture.file().publication().written());
        CHECK(commandResult(controller,
                            controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        settle([&] { return !notice->isVisible(); });
        CHECK(!notice->isVisible() && rowLine() != QStringLiteral("Ready to mark resolved"));
        CHECK(commandResult(controller,
                            controller.commands()->redo("choices", QString::number(fixture.file().current().id))));
        settle([&] { return notice->isVisible(); });
        CHECK(notice->isVisible());
    });
    scenes.run("a block around two sections split by a shared line folds until both are decided", [] {
        // splitFile writes one block, lines 2-10, around two sections split by "shared";
        // its marker lines are 2, 6 and 10, and it folds into its row on line 2.
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::splitFile());
        settlePanes(fixture);
        CHECK(linesStarting(fixture, {"<<<<<<<", "=======", ">>>>>>>"}) == QList<int>({2, 6, 10}));
        // Whether each region still has markers, in the controller's order.
        const auto remain = [&] {
            QList<bool> found;
            for (const auto &entry : controller.selectedFile()->comparison()->contributions())
                found.push_back(entry.markersRemain);
            return found;
        };
        const auto folded = [&] {
            const auto blocks = controller.selectedFile()->comparison()->markerBlocks();
            return blocks.size() == 1 && blocks.front().toMap().value("folded").toBool();
        };
        const auto atLaunch = remain();
        CHECK(atLaunch.size() >= 4 && !atLaunch.contains(false) && folded());
        settle([&] { return foldedLines(fixture) == numbers(3, 11); });
        const auto resolve = [&](SourceSide side, std::size_t at) {
            const auto &target = test_support::choiceTarget(fixture.file(), side, ChoiceScope::Section, at);
            CHECK(test_support::previewResult(
                controller,
                controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                    QString::fromStdString(target.source.contributionId),
                                                    ChoicePreviewFlow::Action::ResolveUsingSide)));
            CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
        };
        // Deciding the first section keeps the block, its fold and every region's markers.
        resolve(SourceSide::Stage2, 0);
        CHECK(fixture.text() == QString::fromStdString(test_support::splitText()) && remain() == atLaunch && folded());
        settle([&] { return foldedLines(fixture) == numbers(3, 11); });
        // Deciding the second removes the block, so nothing folds and no region has markers.
        resolve(SourceSide::Stage3, 1);
        CHECK(fixture.text() == QStringLiteral("header\nleft one\nshared\nright two\ntail\n"));
        CHECK(controller.selectedFile()->comparison()->markerBlocks().isEmpty());
        CHECK(remain().size() == atLaunch.size() && !remain().contains(true));
        settle([&] { return foldedLines(fixture).isEmpty(); });
        // One undo brings back the block and its fold.
        CHECK(commandResult(controller,
                            controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.text() == QString::fromStdString(test_support::splitText()) && remain() == atLaunch && folded());
        settle([&] { return foldedLines(fixture) == numbers(3, 11); });
    });
    scenes.run("Take both on a folded row previews either order, and the result gutter has none", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::markedFile());
        settlePanes(fixture);
        expectLook(fixture, "at launch", markedFolded());
        // The row carries Take both, so the result gutter's decision column offers none.
        for (auto *child : fixture.item("resultGutter")->findChildren<QObject *>())
            CHECK(!child->objectName().startsWith(QStringLiteral("resultTakeBoth")));
        const auto before = captureWork(fixture, *controller.selectedFile());
        const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        const auto id = target.source.contributionId;
        const auto button = "resultTakeBoth-" + id;
        for (const bool leftFirst : {true, false}) {
            const auto entry = std::string(leftFirst ? "resultTakeBothLeftFirst-" : "resultTakeBothRightFirst-") + id;
            // Closing the preview can move this row while its modal overlay fades out.
            test_support::waitForLayout(fixture.window, QString::fromStdString(button), 60);
            clickMenuEntry(fixture, button.c_str(), entry.c_str());
            // The order goes through the same replacement preview as the source gutters' menus.
            auto *dialog = fixture.object("replacementDialog");
            settle([&] { return dialog->property("visible").toBool(); });
            const auto replacement = controller.choices()->choicePreview().value("replacement").toString();
            const auto left = replacement.indexOf("left first");
            const auto right = replacement.indexOf("right first");
            if (left < 0 || right < 0 || (left < right) != leftFirst)
                throw std::runtime_error("the " + entry + " preview replaces with " + replacement.toStdString());
            CHECK(QMetaObject::invokeMethod(dialog, "reject"));
            CHECK(controller.choices()->choicePreview().isEmpty());
            // Rejection clears the preview immediately; the modal closes asynchronously.
            settle([&] { return !dialog->property("visible").toBool(); });
            checkUnchanged(fixture, *controller.selectedFile(), before);
        }
        // Hiding the source choices hides Take both too; the row stays.
        click(fixture, "showSourceChoices");
        settle(
            [&] { return !test_support::visualItem(fixture.window->contentItem(), QString::fromStdString(button)); });
        expectLook(fixture, "with the choices hidden", markedFolded());
        checkUnchanged(fixture, *controller.selectedFile(), before);
    });
    scenes.run("markerless drafts retain decisions without conflict navigation", [] {
        const std::string result = "same\nAAA\nmid\nBBB\n";
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::choiceFile("same\nleftA\nmid\nleftB\n", result, result));
        CHECK(fixture.file().facts().undecided == 4);
        const auto noConflicts = [&] {
            settle([&] {
                return fixture.item("undecidedCounter")->property("text").toString()
                    == QStringLiteral("No conflicts to step through");
            });
            for (const auto *name : {"previousUndecided", "undecidedCounter", "nextUndecided"})
                CHECK(!fixture.item(name)->isEnabled());
            CHECK(controller.selectedFile()->progress().value("conflicts").toInt() == 0);
        };
        noConflicts();
        const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        CHECK(test_support::previewResult(
            controller,
            controller.choices()->previewChoice("choices", QString::number(fixture.file().current().id),
                                                QString::fromStdString(target.source.contributionId),
                                                ChoicePreviewFlow::Action::Ignore)));
        CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
        CHECK(fixture.file().facts().undecided == 3);
        CHECK(fixture.text().toStdString() == result);
        noConflicts();
        CHECK(commandResult(controller,
                            controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().facts().undecided == 4);
        CHECK(fixture.text().toStdString() == result);
        noConflicts();
    });
    scenes.run("conflict controls navigate whole marker regions independently of decisions", [] {
        Application controller(test_support::windowConfig());
        Fixture fixture(controller, test_support::splitFile());
        const auto before = fixture.file().current();
        const auto marked = test_support::splitText();
        const auto begin = static_cast<int>(marked.find("<<<<<<<"));
        const auto end = static_cast<int>(marked.find(">>>>>>> theirs") + std::string(">>>>>>> theirs\n").size());
        CHECK(fixture.file().facts().undecided > 1);
        CHECK(fixture.file().conflictRegions().size() == 1);
        settle([&] { return fixture.item("undecidedCounter")->property("text").toString() == "1 conflict left"; });
        select(fixture, 0, 0);
        for (const auto *name : {"nextUndecided", "undecidedCounter", "previousUndecided"}) {
            CHECK(fixture.item(name)->isEnabled());
            click(fixture, name);
            expectNavigation(fixture, *controller.navigator(), *controller.selection(), "conflict", "choices", "result",
                             0, 1, begin, end,
                             QStringLiteral("Conflict 1 of 1 · result bytes %1-%2").arg(begin).arg(end));
            CHECK(fixture.item("resultText")->hasActiveFocus());
        }
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
    });
}
