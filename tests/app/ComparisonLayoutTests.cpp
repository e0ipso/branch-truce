// Layout of the comparison window: pane widths, headers, tooltips, the footer, the toolbar and its
// overflow, and the earlier version.
#include "ComparisonScene.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"

using test_support::commandResult;

using namespace comparison_scene;

void comparison_scene::addLayoutCases(Groups groups)
{
    auto &scenes = groups.scenes;
    scenes.run("native reading widths, path weights and row insets remain balanced", [] {
        auto sources = *test_support::batchFile().snapshots();
        sources.path = "src/net/retry.cpp";
        const auto launch = sources.launchResult.text.utf8;
        Fixture fixture(
            FileSession("choices", sources.path, {launch, {}, 0}, {.snapshots = sources, .initializeChoices = true}));
        const auto before = captureWork(fixture);
        for (const auto height : {720, 800}) {
            fixture.window->resize(1280, height);
            settlePanes(fixture);
            const auto left = fixture.item("stage2Viewport")->width();
            const auto result = fixture.item("resultViewport")->width();
            const auto right = fixture.item("stage3Viewport")->width();
            CHECK(std::max({left, result, right}) - std::min({left, result, right}) <= 1.001);
        }
        const auto directory = fixture.item("selectedPathDirectory")->property("font").value<QFont>();
        const auto basename = fixture.item("selectedPathBasename")->property("font").value<QFont>();
        CHECK(directory.weight() == QFont::Normal);
        CHECK(basename.weight() == QFont::DemiBold);
        auto *list = fixture.item("fileList");
        auto *row = test_support::visualButton(
            list, [](QQuickItem *item) { return item->property("fileId").toString() == QStringLiteral("choices"); });
        CHECK(row);
        const auto plate = row->mapRectToScene(row->boundingRect());
        const auto outer = list->parentItem()->mapRectToScene(list->parentItem()->boundingRect());
        const auto leading = plate.left() - outer.left();
        const auto trailing = outer.right() - plate.right();
        CHECK(leading > 0 && trailing > 0 && std::abs(leading - trailing) < 0.5);
        row->forceActiveFocus(Qt::TabFocusReason);
        CHECK(row->hasActiveFocus() && test_support::reachable(fixture.window, row));
        checkUnchanged(fixture, before);
    });
    scenes.run("native identity tooltips render literal text through style wrappers", [] {
        Fixture fixture;
        settlePanes(fixture);
        const auto before = captureWork(fixture);
        const auto literal = QStringLiteral("<b>literal identity & source</b>");
        for (const auto *side : {"stage2", "stage3"}) {
            const auto identityName = QString::fromLatin1(side) + QStringLiteral("Identity");
            auto *identity = fixture.item(identityName.toUtf8().constData());
            auto *tooltip = identity->findChild<QObject *>(identityName + QStringLiteral("ToolTip"));
            CHECK(tooltip && tooltip->setProperty("text", literal));
            CHECK(QMetaObject::invokeMethod(tooltip, "open"));
            settleFor("identity tooltip opened", [&] { return tooltip->property("opened").toBool(); });
            int labels = 0;
            const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
                if (item->property("textFormat").isValid() && item->property("text").toString() == literal) {
                    ++labels;
                    CHECK(item->property("textFormat").toInt() == Qt::PlainText);
                }
                for (auto *child : item->childItems()) visit(child);
            };
            auto *content = tooltip->property("contentItem").value<QQuickItem *>();
            CHECK(content);
            visit(content);
            CHECK(labels > 0);
            CHECK(QMetaObject::invokeMethod(tooltip, "close"));
            settleFor("identity tooltip closed", [&] { return !tooltip->property("visible").toBool(); });
        }
        checkUnchanged(fixture, before);
    });
    scenes.run("source choices overlay full-width panes only on hover or focus", [] {
        Fixture fixture;
        settlePanes(fixture);
        const auto before = captureWork(fixture);
        auto *comparison = fixture.item("comparisonView");
        for (const auto *side : {"stage2", "stage3"}) {
            const auto prefix = std::string(side);
            auto *viewport = fixture.item((prefix + "Viewport").c_str());
            CHECK(std::abs(viewport->x()) < 0.01);
            CHECK(std::abs(viewport->width() - viewport->parentItem()->width()) < 0.01);
            CHECK(fixture.item((prefix + "Backgrounds").c_str())->width() >= viewport->width());
            auto *gutter = fixture.item((prefix + "Gutter").c_str());
            CHECK(std::abs(gutter->mapToScene(QPointF()).x() - viewport->mapToScene(QPointF()).x()) < 0.01);
            CHECK(!test_support::visualItem(fixture.window->contentItem(),
                                            QString::fromStdString(prefix + "ChoiceColumn")));
            const auto width = viewport->width();
            CHECK(comparison->setProperty("showChoices", false));
            CHECK(std::abs(viewport->width() - width) < 0.01);
            CHECK(comparison->setProperty("showChoices", true));
        }
        const auto &choice = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
        const auto name = QStringLiteral("choiceAccept-") + QString::fromStdString(choice.source.contributionId);
        auto *lanes = fixture.item("scopedChoices");
        auto *button = test_support::reachableButton(fixture.window, lanes, name);
        CHECK(button);
        // Repeater rows can belong to a separate QObject tree even while
        // their buttons are in this window's visual tree. Use the real owner.
        auto *menu = button->parentItem()->findChild<QObject *>(
            QString::fromStdString("choiceMore-" + choice.source.contributionId));
        CHECK(menu);
        const auto waitOverlay = [&](const char *phase, const std::function<bool()> &ready) {
            try {
                settleFor(phase, ready);
            } catch (const std::exception &error) {
                auto *row = button->parentItem();
                auto *pane = fixture.item("stage2Viewport")->parentItem()->parentItem();
                auto *focus = fixture.window->activeFocusItem();
                throw std::runtime_error(std::string(error.what()) + "; sourceHoveredLine="
                                         + std::to_string(pane->property("hoveredLine").toInt())
                                         + " rowHovered=" + std::to_string(row->property("rowHovered").toBool())
                                         + " revealed=" + std::to_string(row->property("revealed").toBool())
                                         + " choiceFocused=" + std::to_string(row->property("choiceFocused").toBool())
                                         + " activeFocus=" + (focus ? focus->objectName().toStdString() : "none"));
            }
        };
        QTest::mouseMove(fixture.window, QPoint(5, 5));
        fixture.item("resultText")->forceActiveFocus();
        waitOverlay("source overlay idle before touch", [&] { return !test_support::painted(button); });
        const auto center = QPointF(button->width() / 2, button->height() / 2);
        CHECK(!button->contains(center));
        static auto *touch = QTest::createTouchDevice();
        const auto point = button->mapToScene(center).toPoint();
        QTest::touchEvent(fixture.window, touch).press(0, point, fixture.window);
        QTest::touchEvent(fixture.window, touch).release(0, point, fixture.window);
        checkUnchanged(fixture, before);
        CHECK(!menu->property("visible").toBool());
        // Hover the code away from the buttons. The same source observer
        // must reveal the overlay and retain source/counterpart highlighting.
        auto *viewport = fixture.item("stage2Viewport");
        const auto rowY = button->mapToItem(viewport, center).y();
        QTest::mouseMove(fixture.window, viewport->mapToScene(QPointF(viewport->width() / 2, rowY)).toPoint());
        waitOverlay("source code hover reveals its overlay", [&] {
            auto *row = button->parentItem();
            auto *pane = viewport->parentItem()->parentItem();
            return pane->property("hoveredLine").toInt() == row->property("modelData").toMap().value("line").toInt()
                && row->property("rowHovered").toBool() && test_support::painted(button);
        });
        CHECK(test_support::hoverChoiceControl(fixture.window, lanes, name) == button);
        CHECK(test_support::painted(button));
        CHECK(button->contains(center));
        auto *bar = fixture.item("stage2VerticalScrollBar");
        if (bar->isVisible())
            CHECK(button->mapRectToScene(button->boundingRect()).right()
                  <= bar->mapRectToScene(bar->boundingRect()).left());
        QTest::mouseMove(fixture.window, QPoint(5, 5));
        waitOverlay("source overlay hidden after leaving hover", [&] { return !test_support::painted(button); });
        // Traverse the real native Tab chain; opacity must not remove the target.
        auto *previous = button->nextItemInFocusChain(false);
        CHECK(previous && previous != button);
        CHECK(previous != fixture.item("resultText"));
        previous->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(fixture.window, Qt::Key_Tab);
        waitOverlay("source overlay reached by native Tab", [&] { return button->hasActiveFocus(); });
        CHECK(test_support::painted(button));
        QTest::keyClick(fixture.window, Qt::Key_Menu);
        waitOverlay("source overlay keyboard menu opened", [&] { return menu->property("visible").toBool(); });
        CHECK(test_support::painted(button));
        QTest::keyClick(fixture.window, Qt::Key_Escape);
        checkUnchanged(fixture, before);
    });
    scenes.run("pane headers do not rearrange recursively", [] {
        const test_support::WarningCapture rearranges(
            [](const QString &message) { return message.contains(QStringLiteral("Detected recursive rearrange")); });
        Fixture fixture;
        fixture.window->resize(960, 540);
        settle([&] { return fixture.item("stage2Label")->width() > 0; });
        fixture.window->resize(1500, 1100);
        test_support::waitForFrames(fixture.window, "the resized headers are laid out");
        CHECK_EQ(rearranges.count(), 0);
    });
    scenes.run("source headers resolve the current conflict from their side", [] {
        Fixture fixture(test_support::markedFile());
        settlePanes(fixture);
        const auto header = [&](const char *label) {
            auto *item = fixture.item(label);
            while (item && !item->property("trailingRoom").isValid()) item = item->parentItem();
            CHECK(item);
            return item;
        };
        const auto button = [&](const char *side) {
            return fixture.item((std::string(side) + "ResolveConflict").c_str());
        };
        const auto resultHeight = header("resultLabel")->height();
        const auto checkLayout = [&](const char *side) {
            const auto prefix = std::string(side);
            auto *paneHeader = header((prefix + "Label").c_str());
            CHECK(std::abs(paneHeader->height() - resultHeight) < 0.5);
            const auto bounds = paneHeader->mapRectToScene(paneHeader->boundingRect());
            const auto own = button(side)->mapRectToScene(button(side)->boundingRect());
            CHECK(test_support::reachable(fixture.window, button(side)) && bounds.contains(own));
            // The label shows only where the title and the full identity still fit.
            const auto labelled = button(side)->property("display").toInt() == 2; // AbstractButton.TextBesideIcon
            if (labelled) CHECK(!fixture.item((prefix + "Identity").c_str())->property("truncated").toBool());
            return labelled;
        };
        const auto shown = [&](bool labelled) {
            for (const auto *side : {"stage2", "stage3"})
                if ((button(side)->property("display").toInt() == 2) != labelled) return false;
            return true;
        };
        CHECK(checkLayout("stage2") && checkLayout("stage3"));
        fixture.window->resize(960, 540);
        settleFor("icon-only header choices", [&] { return shown(false); });
        CHECK(!checkLayout("stage2") && !checkLayout("stage3"));
        fixture.window->resize(1500, 1100);
        settleFor("labelled header choices", [&] { return shown(true); });
        CHECK(checkLayout("stage2") && checkLayout("stage3"));

        const auto enabled = [&](bool left, bool right) {
            return button("stage2")->isEnabled() == left && button("stage3")->isEnabled() == right;
        };
        const auto cursorAt = [&](const char *text) {
            const auto at = static_cast<int>(fixture.text().indexOf(QString::fromLatin1(text)));
            CHECK(at >= 0);
            fixture.select(at, at);
        };
        // The two sections in result order, and each side's decision on one.
        QStringList sections;
        for (const auto &entry : fixture.controller.selectedFile()->regions()) {
            const auto region = entry.toMap();
            const auto id = region.value("section").toString();
            if (region.value("scope").toString() == "section" && !sections.contains(id)) sections.push_back(id);
        }
        CHECK(sections.size() == 2);
        const auto state = [&](const QString &section, const char *side) {
            for (const auto &entry : fixture.controller.selectedFile()->regions()) {
                const auto region = entry.toMap();
                if (region.value("section").toString() == section && region.value("scope").toString() == "section"
                    && region.value("side").toString() == QLatin1String(side))
                    return region.value("state").toString();
            }
            return QString();
        };
        const auto decided = [&](const QString &section, const char *accepted, const char *ignored) {
            return state(section, accepted) == "accepted" && state(section, ignored) == "ignored";
        };
        const auto launch = fixture.file().current();
        // The conflict at the cursor comes first, even with an earlier one undecided.
        cursorAt("left later");
        settleFor("both sides can resolve", [&] { return enabled(true, true); });
        clickItem(fixture, button("stage2"));
        settleFor("the second conflict resolved using Left", [&] { return decided(sections[1], "stage2", "stage3"); });
        CHECK(state(sections[0], "stage2") == "undecided" && state(sections[0], "stage3") == "undecided");
        CHECK(!fixture.object("replacementDialog")->property("visible").toBool());
        // Outside every conflict the headers take the next undecided one, wrapping to the first.
        cursorAt("tail");
        settleFor("the first conflict is next", [&] { return enabled(true, true); });
        CHECK(test_support::accessibleName(button("stage3"))
              == QStringLiteral("Resolve the current conflict using Right"));
        clickItem(fixture, button("stage3"));
        settleFor("the first conflict resolved using Right", [&] { return decided(sections[0], "stage3", "stage2"); });
        CHECK(fixture.controller.selectedFile()->markerBlocks().isEmpty());
        CHECK(fixture.text() == QStringLiteral("header\nright first\nright second\nbridge\nleft later\ntail\n"));
        // With no undecided conflict left, neither header can resolve one.
        settleFor("nothing left to resolve", [&] { return enabled(false, false); });
        for (int i = 0; i < 2; ++i)
            CHECK(commandResult(
                fixture.controller,
                fixture.controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().current().draft == launch.draft);
        // In a narrow header the complete identity outranks the title: the title elides
        // and the button stays inside the header.
        fixture.window->resize(1280, 720);
        // The native replay's identity, with a title far wider than the header.
        auto *narrow = header("stage3Label");
        CHECK(narrow->setProperty("title", QString(80, QLatin1Char('t')))
              && narrow->setProperty("identityRole", QStringLiteral("Merged commit"))
              && narrow->setProperty("identityHash", QStringLiteral("017b782636"))
              && narrow->setProperty("identity", QString::fromUtf8("Merged commit \u00b7 017b782636")));
        settleFor("a long title in a narrow header", [&] {
            const auto bounds = narrow->mapRectToScene(narrow->boundingRect());
            return bounds.right() <= fixture.window->width() + 0.5
                && fixture.item("stage3Label")->property("truncated").toBool();
        });
        CHECK(!fixture.item("stage3Identity")->property("truncated").toBool());
        checkLayout("stage3");
    });
    scenes.run("pane budget uses the laid-out editor row", [] {
        Fixture fixture(test_support::choiceFile("left\n", "right\n", "result\n"));
        auto *editor = fixture.item("resultText");
        auto *document = editor->property("textDocument").value<QQuickTextDocument *>();
        CHECK(document && document->textDocument());
        auto *text = document->textDocument();
        settle([&] {
            return text->begin().blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight
                && text->lastBlock().blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight;
        });
        const auto first = sceneRectangle(editor, text->begin().position());
        const auto second = sceneRectangle(editor, text->begin().next().position());
        const auto row = second.y() - first.y();
        const auto budgetRow = fixture.item("comparisonView")->property("rowHeight").toReal();
        CHECK(row > 0 && std::abs(budgetRow - std::ceil(row)) < 0.01);
        const auto *blocks = fixture.item("resultGutter")->property("blocks").value<branch_truce::TextBlocks *>();
        CHECK(blocks && std::abs(budgetRow - std::ceil(blocks->rowHeight())) < 0.01);
        CHECK(!fixture.controller.selectedFile()->mimeIcon().isEmpty());
        CHECK(!fixture.item("selectedTypeIcon")->property("source").toString().isEmpty());
    });
    scenes.run("spaced rows preserve work through wrapping, typing and history", [] {
        const std::string longLine(600, 'x');
        const std::string original = longLine + "\nsecond\n";
        Fixture fixture(test_support::diff3File(original, original, original, original));
        settlePanes(fixture);
        const auto before = captureWork(fixture);
        CHECK(fixture.item("showEarlier")->setProperty("checked", true));
        CHECK(fixture.item("wrapLines")->setProperty("checked", true));
        const auto expected = fixture.item("comparisonView")->property("rowHeight").toReal();
        for (const auto *name : {"stage2Text", "resultText", "stage3Text", "earlierText"}) {
            auto *editor = fixture.item(name);
            auto *doc = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
            settle([&] { return visualLines(editor, 0) > 1; });
            const auto first = doc->begin();
            auto *layout = first.layout();
            for (int row = 1; row < layout->lineCount(); ++row) {
                const auto a = sceneRectangle(editor, first.position() + layout->lineAt(row - 1).textStart());
                const auto b = sceneRectangle(editor, first.position() + layout->lineAt(row).textStart());
                CHECK(std::abs((b.y() - a.y()) - expected) < 0.01);
            }
            const auto last =
                sceneRectangle(editor, first.position() + layout->lineAt(layout->lineCount() - 1).textStart());
            const auto next = sceneRectangle(editor, first.next().position());
            CHECK(std::abs(next.y() - last.y() - expected) < 0.01);
            CHECK(expected > last.height());
            CHECK(!doc->isUndoRedoEnabled() && doc->availableUndoSteps() == 0);
        }
        checkUnchanged(fixture, before);
        fixture.checkRecovered(before.draft);
        fixture.select(0, 0);
        fixture.key(Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        fixture.key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        const auto edited = captureWork(fixture);
        CHECK(edited.draft.text == "a\n" + original);
        CHECK(edited.draft.decisions == before.draft.decisions);
        fixture.checkRecovered(edited.draft);
        CHECK(commandResult(
            fixture.controller,
            fixture.controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().current().draft.text == "a" + original);
        CHECK(commandResult(
            fixture.controller,
            fixture.controller.commands()->undo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().current().draft == before.draft);
        CHECK(commandResult(
            fixture.controller,
            fixture.controller.commands()->redo("choices", QString::number(fixture.file().current().id))));
        CHECK(commandResult(
            fixture.controller,
            fixture.controller.commands()->redo("choices", QString::number(fixture.file().current().id))));
        CHECK(fixture.file().current().draft == edited.draft);
        settle([&] { return visualLines(fixture.item("resultText"), 1) > 1; });
        auto &document = resultDocument(fixture);
        settle([&] {
            for (auto block = document.begin(); block.isValid(); block = block.next())
                if (block.blockFormat().lineHeightType() != QTextBlockFormat::LineDistanceHeight) return false;
            return true;
        });
        for (auto block = document.begin(); block.isValid(); block = block.next())
            CHECK(block.blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight);
        CHECK(document.availableUndoSteps() == 0);
        fixture.checkRecovered(edited.draft);
    });
    scenes.run("recovery failures stay visible on unselected file rows", [] {
        Fixture fixture;
        CHECK(fixture.controller.store()->appendFile(namedChoiceFile("other")));
        const auto original = fixture.text();
        fixture.select(0, 0);
        {
            const test_support::ArmedFault fault(*fixture.faults, test_support::SyncFaults::Point::JournalSync);
            fixture.key(Qt::Key_A, Qt::NoModifier, "a");
            // Typing is recorded behind the editor, once the burst ends.
            settle([&] { return fixture.controller.files()->hasPendingRecovery(); });
            CHECK_EQ(fixture.faults->failures.load(), 1);
        }
        settle([&] { return fixture.controller.files()->hasPendingRecovery(); });
        const std::function<QQuickItem *(QQuickItem *)> rowFor = [&](QQuickItem *item) -> QQuickItem * {
            if (item->property("fileId").toString() == QStringLiteral("choices")
                && item->property("subtitle").isValid())
                return item;
            for (auto *child : item->childItems())
                if (auto *found = rowFor(child)) return found;
            return nullptr;
        };
        auto *row = rowFor(fixture.item("fileList"));
        CHECK(row);
        const auto failure = fixture.controller.selectedFile()->status();
        CHECK(failure.contains(QStringLiteral("Recovery failed")));
        settle([&] { return row->property("subtitle").toString() == failure; });
        CHECK(fixture.controller.selection()->select("other"));
        CHECK(row->property("subtitle").toString() == failure);
        CHECK(!fixture.controller.editor()->editable());
        CHECK(fixture.controller.selection()->select("choices"));
        test_support::waitForLayout(fixture.window, QStringLiteral("retryRecovery"), 60);
        fixture.clickLaidOut("retryRecovery");
        settle([&] { return !fixture.controller.files()->hasPendingRecovery(); });
        CHECK(row->property("subtitle").toString() != failure);
        CHECK(fixture.text() == QStringLiteral("a") + original);
        fixture.checkRecovered(fixture.file().current().draft, 2);
    });
    scenes.run("the footer projects captured text format without editing the buffer", [] {
        auto sources = *test_support::choiceFile().snapshots();
        sources.launchResult.text.format.encoding = SnapshotFormat::Encoding::Utf16LittleEndian;
        sources.launchResult.text.format.newline = SnapshotFormat::Newline::CrLf;
        sources.launchResult.text.format.bom = true;
        const auto encoded = storage::serializeText(
            QByteArray::fromStdString(sources.launchResult.text.utf8),
            {SnapshotFormat::Encoding::Utf16LittleEndian, true, SnapshotFormat::Newline::CrLf, true});
        CHECK(encoded.document);
        sources.launchResult.text.bytes = encoded.document->originalBytes.toStdString();
        sources.launchResult.identity.size = encoded.document->originalBytes.size();
        Fixture fixture(FileSession("choices", "choices.txt", {sources.launchResult.text.utf8, {}, 0},
                                    {.snapshots = sources, .initializeChoices = true}));
        const auto before = captureWork(fixture);
        CHECK(fixture.controller.selectedFile()->formatDescription() == QStringLiteral("UTF-16 LE with BOM · CRLF"));
        CHECK(fixture.item("fileFacts")
                  ->property("text")
                  .toString()
                  .contains(QStringLiteral("UTF-16 LE with BOM · CRLF")));
        checkUnchanged(fixture, before);
    });
    scenes.run("header operation and footer progress stay separate", [] {
        Fixture fixture;
        auto *subtitle = fixture.item("repositorySubtitle");
        CHECK(subtitle->property("text").toString() == fixture.controller.loader()->operationDescription());
        CHECK(fixture.item("workflowStatus")->property("text").toString() == QStringLiteral("0 of 1 file resolved"));
        CHECK(fixture.controller.store()->appendFile(FileSession(
            "outside", "diagram.png", {}, {.support = Support::OutsideApp, .supportDetail = "image content"})));
        settle([&] {
            return fixture.item("workflowStatus")->property("text").toString()
                == QStringLiteral("0 of 2 files resolved");
        });
        CHECK(subtitle->property("text").toString() == fixture.controller.loader()->operationDescription());
    });
    scenes.run("actual comparison keeps verified sources around the editable launch result", [] {
        const std::string launch = "manual launch UTF-8 \xc3\xa9 \xf0\x9f\x98\x80\n";
        Fixture fixture(test_support::choiceFile("left capture\n", "right capture\n", launch));
        const auto before = fixture.file().current();
        CHECK(fixture.text().toStdString() == launch);
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
            const auto *prefix = side == SourceSide::Stage2 ? "stage2" : "stage3";
            const auto &source =
                side == SourceSide::Stage2 ? fixture.file().snapshots()->stage2 : fixture.file().snapshots()->stage3;
            CHECK(fixture.item((std::string(prefix) + "Text").c_str())->property("readOnly").toBool());
            CHECK(fixture.item((std::string(prefix) + "Text").c_str())->property("text").toString().toStdString()
                  == source->worktree.utf8);
            const auto label = fixture.item((std::string(prefix) + "Label").c_str())->property("text").toString();
            CHECK(label.contains(QString::fromStdString(source->label)));
            const auto identity = fixture.item((std::string(prefix) + "Identity").c_str())->property("text").toString();
            CHECK(identity == QString::fromStdString(*source->commitId).left(10));
            const auto projected = fixture.controller.selectedFile()->property(prefix).toMap();
            CHECK(projected.value("commitId").toString().toStdString() == *source->commitId);
        }
        CHECK(!fixture.item("resultText")->property("readOnly").toBool());
        CHECK(fixture.item("resultLabel")->property("text").toString() == QStringLiteral("Result"));
        const auto x = [&](const char *name) { return fixture.item(name)->mapToScene({0, 0}).x(); };
        CHECK(x("stage2Text") < x("resultText") && x("resultText") < x("stage3Text"));
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
        fixture.checkRecovered(before.draft);
    });
    scenes.run("toolbar commands report a button role and only the toggles are checkable", [] {
        Fixture fixture;
        // One line per control, so a failure names every control that reports the wrong role.
        const auto describe = [&](const char *name) {
            auto *item = fixture.item(name);
            auto *iface = QAccessible::queryAccessibleInterface(item);
            return QStringLiteral("%1 checkable=%2 role=%3")
                .arg(QLatin1String(name))
                .arg(item->property("checkable").toBool())
                .arg(iface ? static_cast<int>(iface->role()) : -1);
        };
        QStringList commands;
        QStringList expectedCommands;
        for (const auto *name : {"saveDraft", "markResolved", "undoCommand", "redoCommand", "openHandoff",
                                 "previousUndecided", "undecidedCounter", "nextUndecided"}) {
            commands << describe(name);
            expectedCommands << QStringLiteral("%1 checkable=0 role=%2")
                                    .arg(QLatin1String(name))
                                    .arg(static_cast<int>(QAccessible::Button));
        }
        for (const auto *name : {"navigateMenu"}) {
            commands << describe(name);
            expectedCommands << QStringLiteral("%1 checkable=0 role=%2")
                                    .arg(QLatin1String(name))
                                    .arg(static_cast<int>(QAccessible::ButtonMenu));
        }
        QStringList toggles;
        QStringList expectedToggles;
        for (const auto *name : {"toggleFileList"}) {
            toggles << describe(name);
            expectedToggles << QStringLiteral("%1 checkable=1 role=%2")
                                   .arg(QLatin1String(name))
                                   .arg(static_cast<int>(QAccessible::CheckBox));
        }
        test_support::fileToolbarControl(fixture.window, "copyResult");
        for (const auto *name : {"copyResult", "pasteResult", "discardUnsaved", "sourceChoicesMenu"}) {
            commands << describe(name);
            expectedCommands << QStringLiteral("%1 checkable=0 role=%2")
                                    .arg(QLatin1String(name))
                                    .arg(static_cast<int>(QAccessible::MenuItem));
        }
        toggles << describe("showSourceChoices");
        expectedToggles
            << QStringLiteral("showSourceChoices checkable=1 role=%1").arg(static_cast<int>(QAccessible::MenuItem));
        if (commands != expectedCommands || toggles != expectedToggles)
            throw std::runtime_error(
                ("commands:\n" + commands.join('\n') + "\ntoggles:\n" + toggles.join('\n')).toStdString());
        CHECK(commands == expectedCommands && toggles == expectedToggles);
        auto *discard = fixture.item("discardUnsaved");
        auto *discardFace = QAccessible::queryAccessibleInterface(discard);
        CHECK(discardFace && discardFace->role() == QAccessible::MenuItem);
        QTest::keyClick(fixture.window, Qt::Key_Escape);
        settle([&] { return !test_support::namedButton(fixture.window->contentItem(), QStringLiteral("copyResult")); });
        fixture.click("navigateMenu");
        auto *difference = test_support::waitForLayout(fixture.window, QStringLiteral("previousDifference"), 60);
        auto *differenceFace = QAccessible::queryAccessibleInterface(difference);
        CHECK(differenceFace && differenceFace->role() == QAccessible::MenuItem);
        QTest::keyClick(fixture.window, Qt::Key_Escape);
    });
    scenes.run("a named command overflows as a menu item and closes after one activation", [] {
        Fixture fixture(test_support::choiceFile("left\n", "right\n", "original\n"));
        // Both keys are one burst of typing, which one undo takes back.
        const auto beforeTyping = fixture.file().current();
        fixture.type("ab");
        auto *actions = fixture.object("resultEditActions");
        auto *undo = actions->property("undo").value<QObject *>();
        CHECK(undo);
        QSignalSpy triggered(undo, SIGNAL(triggered(QObject *)));
        CHECK(triggered.isValid());
        auto *toolbar = fixture.item("fileToolBar");
        // Constrain the actual toolbar, regardless of the window's minimum width.
        // Save and Mark resolved keep their compact buttons; Undo must overflow.
        QQmlExpression narrow(qmlContext(toolbar), toolbar, QStringLiteral("Layout.maximumWidth = 100"));
        narrow.evaluate();
        CHECK(!narrow.hasError());
        auto *more = test_support::waitForStable(
            fixture.window, "file toolbar overflow",
            [&] { return test_support::namedButton(toolbar, QStringLiteral("More Actions")); }, 60);
        clickItem(fixture, more);
        auto *entry = test_support::waitForStable(
            fixture.window, "overflowed Undo",
            [&] { return test_support::namedButton(fixture.window->contentItem(), QStringLiteral("undoCommand")); },
            60);
        CHECK(entry->objectName() == "undoCommand");
        auto *menu = entry->property("containingMenu").value<QObject *>();
        CHECK(menu && menu->property("visible").toBool());
        auto *accessible = QAccessible::queryAccessibleInterface(entry);
        CHECK(accessible && accessible->role() == QAccessible::MenuItem);
        CHECK(accessible->text(QAccessible::Name) == "Undo");
        clickItem(fixture, entry);
        settle([&] { return !menu->property("visible").toBool(); });
        CHECK(triggered.count() == 1);
        CHECK(fixture.file().current().draft == beforeTyping.draft);
    });
    scenes.run("resizing earlier context and collapsed choices preserve current sources and manual work", [] {
        Fixture fixture;
        CHECK(commandResult(fixture.controller,
                            fixture.controller.commands()->editResult("choices", "1", 0, 0, "manual prefix\n")));
        const auto before = fixture.file().current();
        const auto left = fixture.item("stage2Text")->property("text");
        const auto right = fixture.item("stage3Text")->property("text");
        CHECK(!fixture.item("earlierText")->isVisible());
        CHECK(fixture.item("showEarlier")->property("enabled").toBool());
        CHECK(fixture.item("showEarlier")->setProperty("checked", true));
        settle([&] { return fixture.item("earlierText")->isVisible(); });
        CHECK(fixture.item("earlierText")->property("readOnly").toBool());
        CHECK(fixture.item("earlierText")->property("text").toString().toStdString()
              == fixture.file().snapshots()->earlier->worktree.utf8);
        // The source choices start shown in the gutters; collapse them for the resize below.
        CHECK(fixture.item("scopedChoices")->isVisible());
        fixture.click("showSourceChoices");
        settle([&] { return !fixture.item("scopedChoices")->isVisible(); });
        CHECK(fixture.item("showEarlier")->setProperty("checked", false));
        auto *pane = fixture.item("stage2Viewport")->parentItem()->parentItem();
        const auto width = pane->width();
        QQmlExpression resize(qmlContext(pane), pane, QStringLiteral("SplitView.preferredWidth = %1").arg(width + 80));
        resize.evaluate();
        if (resize.hasError()) throw std::runtime_error(resize.error().toString().toStdString());
        settle([&] { return pane->width() > width + 20; });
        fixture.window->resize(1700, 1150);
        settle([&] { return fixture.item("comparisonPanes")->width() > 1200; });
        CHECK(fixture.item("stage2Text")->property("text") == left
              && fixture.item("stage3Text")->property("text") == right);
        CHECK(!fixture.item("earlierText")->isVisible() && !fixture.item("scopedChoices")->isVisible());
        CHECK(fixture.text().toStdString() == before.draft.text);
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
        fixture.checkRecovered(before.draft);
    });
    scenes.run("unavailable earlier context stays optional and leaves both current sources visible", [] {
        Fixture fixture(test_support::choiceFile());
        const auto before = fixture.file().current();
        CHECK(!fixture.item("showEarlier")->property("enabled").toBool());
        CHECK(fixture.item("showEarlier")->property("text").toString().contains("unavailable"));
        CHECK(fixture.item("showEarlier")->setProperty("checked", true));
        CHECK(!fixture.item("earlierText")->isVisible());
        CHECK(fixture.item("stage2Text")->isVisible() && fixture.item("stage3Text")->isVisible());
        CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
    });
}
