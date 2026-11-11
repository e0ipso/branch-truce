// Window journeys: fit and reachability, the first undecided region, hand edits, line choices
// inside one conflict and the close guard.
#include "acceptance/JourneyData.h"
#include "acceptance/JourneySteps.h"
#include "acceptance/JourneySuite.h"
#include "acceptance/WindowChecks.h"
#include "fixtures/FileToolbar.h"
#include "fixtures/QuickScene.h"
#include "fixtures/SyncFaults.h"
#include "fixtures/UiEvidence.h"

#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QQmlComponent>
#include <QQmlExpression>
#include <QSignalSpy>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace branch_truce;
using test_support::settleFor;

namespace journey {
namespace {

std::string size(const QSize &screen) { return std::to_string(screen.width()) + "x" + std::to_string(screen.height()); }

// --- outside-app placeholders --------------------------------------------------------------

// The compact binary placeholder stays reachable and within its reading width.
void compactPlaceholderIsReachable(Scene &scene)
{
    clickFile(scene, "image.bin");
    for (const int height : {720, 800}) {
        scene.window()->resize(1280, height);
        auto *message = test_support::waitForLayout(scene.window(), QStringLiteral("selectedProblem"), Scene::settleMs);
        CHECK(test_support::reachable(scene.window(), message));
        CHECK(message->width() <= message->parentItem()->property("maximumReadingWidth").toReal());
        test_support::retainUiEvidence(scene.window(), QStringLiteral("placeholder-compact-%1").arg(height),
                                       {{"stage", "binary placeholder"}, {"synthetic_stress", false}});
    }
}

// One window size with deliberate stress text: literal markup, the layout keeps the heading,
// body and Technical details apart, and the details dialog shows the literal path.
void stressPlaceholderAt(Scene &scene, const QSize &screen, const QString &longPath, const QString &heading,
                         const QString &prose)
{
    auto *message = scene.item("selectedProblem");
    auto *placeholder = message->parentItem();
    scene.window()->resize(screen);
    scene.object("pageScroll")->setProperty("contentY", 0);
    test_support::waitForLayout(scene.window(), QStringLiteral("selectedProblem"), Scene::settleMs);
    const auto expectedWidth = std::min(placeholder->property("maximumReadingWidth").toReal(),
                                        placeholder->width() - 2 * placeholder->property("contentMargin").toReal());
    CHECK(std::abs(message->width() - expectedWidth) < 0.5);
    CHECK(std::abs(placeholder->width() - placeholder->parentItem()->width()) < 0.5);
    auto *title = descendant(message, [&](QQuickItem *item) {
        return item->inherits("QQuickText") && item->property("text").toString() == heading;
    });
    auto *body = descendant(message, [&](QQuickItem *item) {
        // Kirigami's selectable explanation uses TextEdit inside a Control wrapper; inspect the
        // actual rendering item.
        return item->inherits("QQuickTextEdit") && item->property("text").toString() == prose;
    });
    CHECK(title && body);
    CHECK(title->property("textFormat").toInt() == 0 && body->property("textFormat").toInt() == 0);
    CHECK(!title->property("truncated").toBool() && !body->property("truncated").toBool());
    CHECK(body->property("lineCount").toInt() > 1);
    CHECK(title->property("contentWidth").toReal() <= title->width() + 1);
    CHECK(body->property("contentWidth").toReal() <= body->width() + 1);
    CHECK(body->property("contentHeight").toReal() <= body->height() + 0.5);
    CHECK(title->mapRectToScene(title->boundingRect()).bottom() <= body->mapRectToScene(body->boundingRect()).top());
    auto *details = stable(scene, "stress placeholder technical details", [&] {
        auto *button = test_support::namedButton(message, QStringLiteral("Technical details"));
        if (button) Scene::reveal(button);
        return test_support::reachable(scene.window(), button) ? button : nullptr;
    });
    CHECK(details->isEnabled());
    CHECK(body->mapRectToScene(body->boundingRect()).bottom()
          <= details->mapRectToScene(details->boundingRect()).top());
    if (screen.height() == 540) {
        CHECK(scene.object("pageScroll")->property("overflowing").toBool());
        CHECK(scene.object("pageScroll")->property("contentY").toReal() > 0);
    }
    QJsonObject evidence{{"stage", "long-path localized-prose stress"},
                         {"synthetic_stress", true},
                         {"stress_description", "synthetic localized text, not a shipped translation"},
                         {"literal_path", longPath},
                         {"heading", heading},
                         {"explanation", prose},
                         {"reading_width", message->width()},
                         {"page_content_y", scene.object("pageScroll")->property("contentY").toReal()}};
    test_support::retainUiEvidence(scene.window(), QStringLiteral("placeholder-stress-%1").arg(screen.height()),
                                   evidence);
    test_support::clickItem(scene.window(), details);
    auto *dialog = scene.object("selectedProblemDetails");
    settleFor("stress technical details opened", [&] { return dialog->property("opened").toBool(); });
    CHECK(scene.text("selectedProblemDiagnosticPath") == longPath);
    CHECK(scene.item("selectedProblemDiagnosticPath")->property("textFormat").toInt() == 0);
    CHECK(scene.item("selectedProblemDiagnostic")->property("readOnly").toBool());
    test_support::retainUiEvidence(scene.window(), QStringLiteral("placeholder-details-%1").arg(screen.height()),
                                   evidence);
    keyClick(scene, Qt::Key_Escape);
    settleFor("stress technical details closed", [&] { return !dialog->property("visible").toBool(); });
}

void outsideAppPlaceholdersKeepStressText(Sandbox &sandbox)
{
    const auto longPath = QStringLiteral("long printable directory/") + QString(90, QLatin1Char('p'))
        + QStringLiteral("/literal <b>report&review</b> ") + QString(70, QLatin1Char('n')) + QStringLiteral(".bin");
    const auto pathBytes = longPath.toUtf8();
    const QByteArray binaryBase("b\0ase", 5), binaryLeft("l\0eft", 5), binaryRight("r\0ght", 5);
    Repository repo({{"story.txt", storyBase, storyLeft, storyRight, storyBase},
                     {"image.bin", binaryBase, binaryLeft, binaryRight, std::nullopt},
                     {longPath, binaryBase, binaryLeft, binaryRight, std::nullopt}});
    const auto repository = repo.snapshot();
    Scene scene(sandbox, repo);
    clickFile(scene, "story.txt");
    const auto before = work(scene, "story.txt");
    const auto revision = scene.file("story.txt").current().id;
    const bool undo = scene.file("story.txt").canUndo(), redo = scene.file("story.txt").canRedo();
    compactPlaceholderIsReachable(scene);
    clickFile(scene, pathBytes.constData());
    auto *placeholder = scene.item("selectedProblem")->parentItem();
    // Deliberate input stress on the real component, not a shipped translation.
    const auto heading = QString::fromUtf8("Synthetischer lokalisierter Hinweis <b>bleibt wörtlich</b>: ")
        + QStringLiteral("Resolve this unusually long printable file name outside Branch Truce");
    const auto prose =
        QString::fromUtf8("Dies ist synthetischer lokalisierter Testtext, keine ausgelieferte Übersetzung. ")
        + QStringLiteral("The literal path is ") + longPath + QStringLiteral(". ")
        + QStringLiteral("Keep this file outstanding until another tool resolves it and Git confirms the exact path. ")
              .repeated(5)
        + QStringLiteral("<i>No markup is interpreted & no history changes.</i>");
    CHECK(placeholder->setProperty("text", heading));
    CHECK(placeholder->setProperty("explanation", prose));
    for (const auto screen : {QSize(1280, 720), QSize(1280, 800), QSize(960, 540)}) {
        stressPlaceholderAt(scene, screen, longPath, heading, prose);
        CHECK(repo.snapshot() == repository);
    }
    clickFile(scene, "story.txt");
    CHECK(work(scene, "story.txt") == before && scene.file("story.txt").current().id == revision);
    CHECK(scene.file("story.txt").canUndo() == undo && scene.file("story.txt").canRedo() == redo);
    CHECK(repo.snapshot() == repository);
    checkOffline(scene);
}

// --- a 1280x720 logical screen fits the window ---------------------------------------------

// Native Labels that resolve the theme's small and fixed-width fonts, since a theme font request
// can leave attributes unresolved on the item that asked. The component lives as long as its item.
struct FontReference {
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QQuickItem> item;
};

FontReference fontReference(Scene &scene)
{
    auto fonts = std::make_unique<QQmlComponent>(&scene.engine());
    fonts->setData(R"(
import QtQuick
import QtQuick.Controls
import org.kde.kirigami as K
Item {
    visible: false
    property alias smallFont: small.font
    property alias fixedFont: fixed.font
    Label { id: small; font: K.Theme.smallFont }
    Label { id: fixed; font: K.Theme.fixedWidthFont }
}
)",
                   QUrl(QStringLiteral("native-font-reference.qml")));
    std::unique_ptr<QQuickItem> reference(qobject_cast<QQuickItem *>(fonts->create()));
    if (!reference) throw std::runtime_error(fonts->errorString().toStdString());
    reference->setParentItem(scene.window()->contentItem());
    return {std::move(fonts), std::move(reference)};
}

void expectAccessibleNames(Scene &scene)
{
    for (const auto *name : mainControls) {
        auto *control = test_support::fileOverflowControl(name)
            ? test_support::fileToolbarControl(scene.window(), name, Scene::settleMs)
            : scene.item(name);
        test_support::requireAccessible(control, name);
        if (test_support::fileOverflowControl(name)) test_support::closeFileToolbarMenu(scene.window());
    }
}

// Moving between files and the file-wide choices sit in toolbar menus. Once a menu opens, each
// entry is a visible item with an accessible name inside the window.
void expectMenusInside(Scene &scene)
{
    const std::pair<const char *, std::vector<const char *>> menus[] = {
        {"navigateMenu", {"previousDifference", "nextDifference", "previousSavedOrReviewed", "nextSavedOrReviewed"}},
        {"sourceChoicesMenu",
         {"nonConflictingLeft", "nonConflictingRight", "nonConflictingBoth", "wholeFileLeft", "wholeFileRight"}},
    };
    for (const auto &[menu, entries] : menus) {
        scene.click(menu);
        for (const auto *entry : entries) {
            auto *item = test_support::waitForLayout(scene.window(), QString::fromLatin1(entry), Scene::settleMs);
            test_support::requireAccessible(item, entry);
            const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            CHECK(QRectF(0, 0, scene.window()->width(), scene.window()->height()).contains(rect));
        }
        keyClick(scene, Qt::Key_Escape);
        settleFor(std::string(menu) + " closed", [&] {
            auto *first = test_support::visualItem(scene.window()->contentItem(), QString::fromLatin1(entries.front()));
            return !first || !first->isVisible();
        });
        if (test_support::fileOverflowControl(menu)) test_support::closeFileToolbarMenu(scene.window());
    }
}

// A font that differs from the native reference, field by field, for the failure log.
void reportFontDifference(QQuickItem *item, const QFont &expected, const QVariant &rawRole, const QVariant &rawHash,
                          bool expressionError)
{
    const auto actual = item->property("font").value<QFont>();
    if (actual == expected) return;
    const auto fields = [](const QFont &font) {
        return QJsonObject{{"key", font.key()},
                           {"family", font.family()},
                           {"styleName", font.styleName()},
                           {"pointSize", font.pointSizeF()},
                           {"pixelSize", font.pixelSize()},
                           {"weight", font.weight()},
                           {"style", font.style()},
                           {"styleHint", font.styleHint()},
                           {"styleStrategy", int(font.styleStrategy())},
                           {"stretch", font.stretch()},
                           {"kerning", font.kerning()},
                           {"fixedPitch", font.fixedPitch()},
                           {"hinting", font.hintingPreference()},
                           {"capitalization", font.capitalization()},
                           {"letterSpacing", font.letterSpacing()},
                           {"wordSpacing", font.wordSpacing()},
                           {"underline", font.underline()},
                           {"overline", font.overline()},
                           {"strikeOut", font.strikeOut()},
                           {"resolveMask", QString::number(static_cast<qulonglong>(font.resolveMask()))}};
    };
    const auto diagnostic = QJsonDocument(QJsonObject{{"control", item->objectName()},
                                                      {"actual", fields(actual)},
                                                      {"expected", fields(expected)},
                                                      {"rawRoleType", QString::fromLatin1(rawRole.metaType().name())},
                                                      {"rawHashType", QString::fromLatin1(rawHash.metaType().name())},
                                                      {"rawRoleValid", rawRole.isValid()},
                                                      {"rawHashValid", rawHash.isValid()},
                                                      {"rawRoleConvertible", rawRole.canConvert<QFont>()},
                                                      {"rawHashConvertible", rawHash.canConvert<QFont>()},
                                                      {"expressionError", expressionError}})
                                .toJson(QJsonDocument::Compact);
    std::fprintf(stderr, "IDENTITY_FONT_DIAGNOSTIC=%s\n", diagnostic.constData());
}

// Each source identity shows its role in the theme's small font and its hash in the fixed-width
// font on one baseline, untruncated, and reads as its whole text.
void expectIdentitiesWhole(Scene &scene, const QQuickItem &reference, const QSize &screen)
{
    for (const auto *identity : {"stage2Identity", "stage3Identity"}) {
        auto *label = scene.item(identity);
        const auto text = label->property("text").toString();
        CHECK(test_support::accessibleName(label) == text);
        CHECK(!text.isEmpty());
        auto *role = scene.item((std::string(identity) + "Role").c_str());
        auto *hash = scene.item((std::string(identity) + "Hash").c_str());
        QQmlExpression smallFont(qmlContext(role), role, "Kirigami.Theme.smallFont");
        QQmlExpression fixedFont(qmlContext(hash), hash, "Kirigami.Theme.fixedWidthFont");
        const auto rawRole = smallFont.evaluate();
        const auto rawHash = fixedFont.evaluate();
        const auto expectedRole = reference.property("smallFont").value<QFont>();
        const auto expectedHash = reference.property("fixedFont").value<QFont>();
        const bool expressionError = smallFont.hasError() || fixedFont.hasError();
        reportFontDifference(role, expectedRole, rawRole, rawHash, expressionError);
        reportFontDifference(hash, expectedHash, rawRole, rawHash, expressionError);
        CHECK(role->property("font").value<QFont>() == expectedRole);
        CHECK(hash->property("font").value<QFont>() == expectedHash);
        CHECK(text.contains(role->property("text").toString()));
        CHECK(text.contains(hash->property("text").toString()));
        CHECK(!hash->property("text").toString().isEmpty());
        CHECK(std::abs(role->mapToScene(QPointF(0, role->baselineOffset())).y()
                       - hash->mapToScene(QPointF(0, hash->baselineOffset())).y())
              < 0.5);
        if (label->property("truncated").toBool()) {
            const auto bounds = label->mapRectToScene(label->boundingRect());
            throw std::runtime_error(
                std::string(identity) + " truncated at " + size(screen) + ": " + text.toStdString() + " bounds="
                + std::to_string(bounds.x()) + "," + std::to_string(bounds.y()) + "," + std::to_string(bounds.width())
                + "," + std::to_string(bounds.height()) + " role=" + role->property("text").toString().toStdString()
                + " roleWidth=" + std::to_string(role->width())
                + " roleImplicit=" + std::to_string(role->implicitWidth())
                + " roleTruncated=" + std::to_string(role->property("truncated").toBool()) + " hash="
                + hash->property("text").toString().toStdString() + " hashWidth=" + std::to_string(hash->width())
                + " hashImplicit=" + std::to_string(hash->implicitWidth())
                + " hashTruncated=" + std::to_string(hash->property("truncated").toBool()));
        }
        CHECK(!label->property("truncated").toBool());
    }
}

// At 1280x800 the handoff dialog wraps between words, and file rows elide one line each instead
// of wrapping, so nothing in the file list wraps at all.
void expectWholeWordsAt1280x800(Scene &scene)
{
    scene.window()->resize(1280, 800);
    settleFor("window at 1280x800", [&] { return scene.window()->width() == 1280 && scene.window()->height() == 800; });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    CHECK(expectWholeWords(scene.item("fileList"), "the file list at 1280x800") == 0);
    auto *dialog = scene.object("handoffDialog");
    scene.click("openHandoff");
    settleFor("handoff opened", [&] { return dialog->property("opened").toBool(); });
    test_support::waitForLayout(scene.window(), QStringLiteral("handoffLeave"), Scene::settleMs);
    // Each blocker has a line of its own, so a row may not need to wrap at all.
    expectWholeWords(dialog->property("contentItem").value<QQuickItem *>(), "the handoff dialog at 1280x800");
    QMetaObject::invokeMethod(dialog, "close");
    settleFor("handoff closed", [&] { return !dialog->property("visible").toBool(); });
}

// Source choices start enabled as hover overlays, reserving no pane width or height: the three
// panes keep eight lines and stay reachable from a roomy window down to 1280x720, and hiding the
// choices leaves every text pane as high as it was.
void sourceChoicesReserveNoSpace(Scene &scene, qreal lineHeight)
{
    settleFor("source choices shown", [&] { return scene.item("scopedChoices")->isVisible(); });
    const std::vector<const char *> withChoices{
        "showSourceChoices", "sourceChoicesMenu", "undecidedCounter", "stage2Label",   "resultLabel",   "stage3Label",
        "stage2Viewport",    "resultViewport",    "stage3Viewport",   "scopedChoices", "workflowStatus"};
    for (const auto &screen : {QSize(1500, 1040), QSize(1280, 800), QSize(1280, 720)}) {
        const auto pane = checkFits(scene, screen.width(), screen.height(), withChoices);
        if (pane < 8 * lineHeight)
            throw std::runtime_error("with source choices, text panes " + std::to_string(pane) + " px high at "
                                     + size(screen) + ", less than eight " + std::to_string(lineHeight) + " px lines");
        clickFileToolbarControl(scene, "showSourceChoices");
        settleFor("source choices hidden at " + size(screen),
                  [&] { return !scene.item("scopedChoices")->isVisible(); });
        const auto hidden =
            checkFits(scene, screen.width(), screen.height(), {"stage2Viewport", "resultViewport", "stage3Viewport"});
        if (hidden != pane)
            throw std::runtime_error("at " + size(screen) + " the text panes are " + std::to_string(pane)
                                     + " px high with the source choices and " + std::to_string(hidden)
                                     + " px without them");
        clickFileToolbarControl(scene, "showSourceChoices");
        settleFor("source choices shown at " + size(screen), [&] { return scene.item("scopedChoices")->isVisible(); });
    }
}

void logicalScreenFitsTheWindow(Sandbox &sandbox)
{
    auto paths = journeyPaths();
    paths.push_back(
        {"image.bin", QByteArray("base\0", 5), QByteArray("left\0", 5), QByteArray("side\0", 5), std::nullopt});
    Repository repo(paths);
    Scene scene(sandbox, repo);
    const auto reference = fontReference(scene);
    CHECK(!scene.item("inventoryWarning")->isVisible() && scene.item("outsideAppCount")->isVisible());
    clickFile(scene, "image.bin");
    CHECK(scene.file("image.bin").support() == Support::OutsideApp);
    expectContains(scene.text("fileFacts"), "5 bytes", "captured binary size");
    clickFile(scene, "story.txt");
    expectContains(scene.text("fileFacts"), "UTF-8 · LF", "captured text format");
    expectAccessibleNames(scene);
    expectMenusInside(scene);
    // A roomy window lays out as before, without scrolling the page.
    CHECK(!scene.object("pageScroll")->property("overflowing").toBool());
    const auto lineHeight = scene.item("comparisonView")->property("rowHeight").toReal();
    CHECK(lineHeight > 0);
    // 1920x1080 at 150% scaling. The strict 960x540 fit requirement is temporarily deferred by the
    // owner.
    for (const auto &screen : {QSize(1280, 720), QSize(1280, 800)}) {
        const auto pane = checkFits(scene, screen.width(), screen.height());
        if (pane < 8 * lineHeight)
            throw std::runtime_error("text panes " + std::to_string(pane) + " px high at " + size(screen)
                                     + ", less than eight " + std::to_string(lineHeight) + " px lines");
        for (const auto *name : {"saveDraft", "markResolved"}) {
            auto *toolbar = test_support::visualItem(scene.window()->contentItem(), "fileToolBar", true);
            CHECK(test_support::reachableButton(scene.window(), toolbar, QString::fromLatin1(name)));
        }
        expectIdentitiesWhole(scene, *reference.item, screen);
        expectMenusInside(scene);
    }
    // Whether this page is taller than a 960x540 window depends on the fonts. It fits with DejaVu
    // Sans 9 pt, as in the CI images. Either way the wheel moves it only when it does not fit.
    scene.window()->resize(960, 540);
    settleFor("window at 960x540", [&] { return scene.window()->width() == 960 && scene.window()->height() == 540; });
    checkWheelScroll(scene, "selectedPath");
    expectWholeWordsAt1280x800(scene);
    sourceChoicesReserveNoSpace(scene, lineHeight);
    checkOffline(scene);
}

// --- selecting a file shows its first undecided region -------------------------------------

void selectionShowsTheFirstUndecidedRegion(Sandbox &sandbox)
{
    QByteArray before;
    QByteArray after;
    for (int line = 1; line <= 120; ++line) before += "shared before " + QByteArray::number(line) + "\n";
    for (int line = 1; line <= 80; ++line) after += "shared after " + QByteArray::number(line) + "\n";
    // Git's conflict output stays the launch result, so the conflict starts at line 121.
    Repository repo({{"long.txt", before + "base value\n" + after, before + "left value\n" + after,
                      before + "right value\n" + after, std::nullopt},
                     {"short.txt", QByteArray("short base\n"), QByteArray("short left\n"), QByteArray("short right\n"),
                      std::nullopt}});
    Scene scene(sandbox, repo);
    // Bounded observations of where the panes stand at each step, for a failure.
    int diagnosticCount = 0;
    std::string lastGeometry;
    const auto recordPlacement = [&](const char *event) {
        if (diagnosticCount >= 16) return;
        const auto geometry = firstUndecidedInView(scene, true);
        if (geometry == lastGeometry) return;
        lastGeometry = geometry;
        ++diagnosticCount;
        std::fprintf(stderr, "PLACEMENT_DIAGNOSTIC event=%s observed=%d\n%s\n", event, diagnosticCount,
                     geometry.c_str());
    };
    QObject placementObserver;
    QObject::connect(scene.app().editor(), &ResultEditorAdapter::placed, &placementObserver,
                     [&] { recordPlacement("editorPlaced"); });
    recordPlacement("before-long-selection");
    clickFile(scene, "long.txt");
    recordPlacement("after-long-selection");
    waitFor(
        "the first undecided region in every pane at launch", [&] { return firstUndecidedInView(scene).empty(); },
        [&] { return firstUndecidedInView(scene); });
    const auto lineHeight = scene.item("comparisonView")->property("rowHeight").toReal();
    CHECK(lineHeight > 0);
    for (const auto &screen : {QSize(1280, 720), QSize(960, 540)}) {
        const auto pane = checkFits(scene, screen.width(), screen.height(),
                                    {"fileList", "stage2Viewport", "resultViewport", "stage3Viewport"});
        if (pane < 8 * lineHeight)
            throw std::runtime_error("text panes " + std::to_string(pane) + " px high at " + size(screen)
                                     + ", less than eight " + std::to_string(lineHeight) + " px lines");
        // The file rows sit at the top of the page. The pointer still rests on long.txt, whose
        // tooltip opens below it over short.txt under the KDE style; clickFile moves and presses at
        // once, so the tooltip must close before the press.
        scene.object("pageScroll")->setProperty("contentY", 0);
        clickFile(scene, "short.txt");
        recordPlacement("before-placement-request");
        clickFile(scene, "long.txt");
        recordPlacement("after-selection-cursor-and-scroll");
        waitFor(
            "the first undecided region in every pane at " + size(screen),
            [&] {
                recordPlacement("observed-layout-cursor-contentY");
                return firstUndecidedInView(scene).empty();
            },
            [&] { return firstUndecidedInView(scene); });
        test_support::waitForFrames(scene.window(), "the journey scene");
        recordPlacement("after-two-rendered-frames");
        CHECK(firstUndecidedInView(scene).empty());
    }
    checkOffline(scene);
}

// --- a choice clears hand edits ------------------------------------------------------------

bool noneEdited(Scene &scene)
{
    const auto regions = scene.regions();
    return std::none_of(regions.begin(), regions.end(), [](const Region &region) { return region.handEdited; });
}

// Puts the cursor at the start of a line of the result.
void cursorToLine(Scene &scene, int line)
{
    auto *editor = scene.item("resultText");
    clickText(scene, editor);
    CHECK(editor->hasActiveFocus());
    keyClick(scene, Qt::Key_Home, Qt::ControlModifier);
    for (int step = 1; step < line; ++step) keyClick(scene, Qt::Key_Down);
}

// Preview whole file from Left, from the Source choices menu.
void chooseWholeLeft(Scene &scene)
{
    scene.click("sourceChoicesMenu");
    test_support::clickItem(
        scene.window(), test_support::waitForLayout(scene.window(), QStringLiteral("wholeFileLeft"), Scene::settleMs));
    clickDialogButton(scene, "wholeFileDialog", "Replace whole file");
}

void choiceClearsHandEdits(Sandbox &sandbox)
{
    // The left side changes two lines in one section, and the user set the result to it before launch.
    const QByteArray left = "header\nleft first\nleft second\nbridge\nleft later\ntail\n";
    Repository repo({{"pair.txt", QByteArray("header\nbase first\nbase second\nbridge\nbase later\ntail\n"), left,
                      QByteArray("header\nright first\nright second\nbridge\nright later\ntail\n"), left}});
    Scene scene(sandbox, repo);
    clickFile(scene, "pair.txt");
    const auto region = [&](const char *label) { return scene.regionFor(QString::fromUtf8(label)); };

    // Deleting a line of the section in the editor leaves its region marked but without a result range.
    cursorToLine(scene, 2);
    keyClick(scene, Qt::Key_Down, Qt::ShiftModifier);
    keyClick(scene, Qt::Key_Delete);
    expectText(scene.text("resultText"), "header\nleft second\nbridge\nleft later\ntail\n", "deleted line");
    CHECK(region("Left line 2").handEdited && !region("Left line 2").mapped);
    const auto deleted = work(scene, "pair.txt");
    // Accepting the section around it puts the line back and clears the mark.
    clickChoice(scene, QString::fromUtf8("Left section, lines 2–3"), "Accept");
    CHECK(scene.item("manualReplacementNotice")->isVisible());
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"), utf8(left), "restored line");
    CHECK(noneEdited(scene));
    CHECK(region("Left line 2").state == QStringLiteral("accepted"));
    const auto restored = work(scene, "pair.txt");
    scene.click("undoCommand");
    CHECK(work(scene, "pair.txt") == deleted && region("Left line 2").handEdited);
    scene.click("redoCommand");
    CHECK(work(scene, "pair.txt") == restored && noneEdited(scene));

    // Typing a character and deleting it again leaves the text as the whole left side but marks it.
    chooseWholeLeft(scene);
    expectText(scene.text("resultText"), utf8(left), "whole left");
    CHECK(noneEdited(scene));
    cursorToLine(scene, 2);
    keyClick(scene, Qt::Key_End);
    typeText(scene, "X");
    keyClick(scene, Qt::Key_Backspace);
    expectText(scene.text("resultText"), utf8(left), "typed and deleted");
    CHECK(!noneEdited(scene));
    const auto manual = work(scene, "pair.txt");
    const auto manualRegions = scene.regions();
    // The same whole-file choice keeps the text and decisions but replaces the edit.
    chooseWholeLeft(scene);
    expectText(scene.text("resultText"), utf8(left), "whole left again");
    CHECK(noneEdited(scene) && scene.file("pair.txt").current().draft == manual.draft);
    scene.click("undoCommand");
    CHECK(work(scene, "pair.txt") == manual);
    CHECK(scene.regions() == manualRegions);
    scene.click("redoCommand");
    CHECK(noneEdited(scene));
    checkOffline(scene);
}

// --- lines from both sides of one multi-line conflict --------------------------------------

constexpr const char *kConflictLines[] = {"Left line 2",  "Left line 3",  "Left line 4",
                                          "Right line 2", "Right line 3", "Right line 4"};

// The decisions of every line of the conflict, left then right, joined by spaces.
void expectLineStates(Scene &scene, const char *expected)
{
    QStringList actual;
    for (const auto *label : kConflictLines) actual.push_back(scene.choiceRow(QString::fromUtf8(label)).state);
    expectText(actual.join(QLatin1Char(' ')), QString::fromUtf8(expected), "line decisions");
}

void linesFromBothSidesKeepTheirDecisions(Sandbox &sandbox)
{
    // Both sides change the same three lines, and the user set the result to the common version before launch.
    const QByteArray base = "header\nbase one\nbase two\nbase three\ntail\n";
    Repository repo({{"lines.txt", base, QByteArray("header\nleft one\nleft two\nleft three\ntail\n"),
                      QByteArray("header\nright one\nright two\nright three\ntail\n"), base}});
    Scene scene(sandbox, repo);
    clickFile(scene, "lines.txt");

    // Every line inside the conflict can be chosen, at the result line with the same index.
    for (const auto *label : kConflictLines) {
        const auto row = scene.choiceRow(QString::fromUtf8(label));
        CHECK(row.available);
        CHECK(row.detail.isEmpty());
    }
    expectText(regionResult(scene, scene.regionFor(QStringLiteral("Left line 3"))), "base two\n", "left line 3 target");
    expectText(regionResult(scene, scene.regionFor(QStringLiteral("Right line 4"))), "base three\n",
               "right line 4 target");
    expectLineStates(scene, "undecided undecided undecided undecided undecided undecided");
    std::vector<Work> history{work(scene, "lines.txt")};

    // The middle line from the left replaces only the launch line at its position.
    clickChoice(scene, "Left line 3", "Accept", Qt::Key_Menu);
    CHECK(scene.item("manualReplacementNotice")->isVisible());
    expectText(scene.text("replacementBeforeText"), "base two\n", "replaced left target");
    expectText(scene.text("replacementAfterText"), "left two\n", "left replacement");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"), "header\nbase one\nleft two\nbase three\ntail\n", "accept left line 3");
    expectLineStates(scene, "undecided accepted undecided undecided undecided undecided");
    history.push_back(work(scene, "lines.txt"));

    // The last line from the right goes beside it in the same conflict.
    clickChoice(scene, "Right line 4", "Accept", Qt::Key_F10);
    expectText(scene.text("replacementBeforeText"), "base three\n", "replaced right target");
    expectText(scene.text("replacementAfterText"), "right three\n", "right replacement");
    clickDialogButton(scene, "replacementDialog", "Apply choice");
    expectText(scene.text("resultText"), "header\nbase one\nleft two\nright three\ntail\n", "accept right line 4");
    expectLineStates(scene, "undecided accepted undecided undecided undecided accepted");
    history.push_back(work(scene, "lines.txt"));

    // Ignore decides against the first left line and keeps the text.
    clickChoice(scene, "Left line 2", "Ignore");
    CHECK(!scene.object("replacementDialog")->property("visible").toBool());
    expectText(scene.text("resultText"), "header\nbase one\nleft two\nright three\ntail\n", "ignore left line 2");
    expectLineStates(scene, "ignored accepted undecided undecided undecided accepted");
    history.push_back(work(scene, "lines.txt"));

    // Undo walks back through text and decisions together, and redo replays them.
    for (std::size_t step = history.size() - 1; step-- > 0;) {
        scene.click("undoCommand");
        CHECK(work(scene, "lines.txt") == history[step]);
    }
    expectLineStates(scene, "undecided undecided undecided undecided undecided undecided");
    CHECK(!scene.file("lines.txt").canUndo() && !scene.item("undoCommand")->isEnabled());
    for (std::size_t step = 1; step < history.size(); ++step) {
        scene.click("redoCommand");
        CHECK(work(scene, "lines.txt") == history[step]);
    }
    expectText(scene.text("resultText"), "header\nbase one\nleft two\nright three\ntail\n", "redone");
    expectLineStates(scene, "ignored accepted undecided undecided undecided accepted");
    checkOffline(scene);
}

// --- closing with input recovery could not keep --------------------------------------------

// Closes the window and requires the recovery close guard instead of the handoff.
void closeIntoTheGuard(Scene &scene)
{
    auto *prompt = scene.object("recoveryCloseDialog");
    auto *handoff = scene.object("handoffDialog");
    scene.window()->close();
    settleFor("close guard opened", [&] { return prompt->property("opened").toBool(); });
    CHECK(scene.window()->isVisible() && !handoff->property("visible").toBool());
}

void closeGuardAsksToRetry(Sandbox &sandbox)
{
    Repository repo({{"kept.txt", QByteArray("kept base\n"), QByteArray("kept left\n"), QByteArray("kept right\n"),
                      QByteArray("kept by hand\n")}});
    const auto launched = repo.snapshot();
    {
        const auto faults = std::make_shared<test_support::SyncFaults>();
        Scene scene(sandbox, repo, faults);
        clickFile(scene, "kept.txt");
        auto &files = *scene.app().store();
        auto *prompt = scene.object("recoveryCloseDialog");
        auto *handoff = scene.object("handoffDialog");
        {
            // Fail the real durable boundary while retaining the exact typed input.
            const test_support::ArmedFault failure(*faults, test_support::SyncFaults::Point::JournalSync);
            appendToResult(scene, "!");
            settleFor("pending recovery", [&] { return files.hasPendingRecovery(); });
            CHECK(faults->failures == 1 && scene.item("recoveryProblem")->isVisible());
            closeIntoTheGuard(scene);
            expectContains(prompt->property("subtitle").toString(), "Retry before closing", "close guard");
            clickDialogButton(scene, "recoveryCloseDialog", "Cancel");
            CHECK(scene.window()->isVisible() && files.hasPendingRecovery() && !handoff->property("visible").toBool());
            // A second failed sync leaves that same input pending and the window open.
            closeIntoTheGuard(scene);
            faults->arm(test_support::SyncFaults::Point::JournalSync);
            test_support::clickItem(scene.window(), dialogButton(scene, "recoveryCloseDialog", "Retry"));
            settleFor("failed recovery retry completed", [&] { return faults->failures == 1 && !files.recording(); });
            CHECK(scene.window()->isVisible() && files.hasPendingRecovery() && !handoff->property("visible").toBool());
            closeIntoTheGuard(scene);
        }
        // With sync succeeding, Retry keeps the input and the close goes on to the handoff.
        clickDialogButton(scene, "recoveryCloseDialog", "Retry");
        settleFor("handoff opened after retry", [&] { return handoff->property("opened").toBool(); });
        CHECK(scene.window()->isVisible() && !files.hasPendingRecovery());
        expectText(QString::fromStdString(scene.file("kept.txt").current().draft.text), "kept by hand\n!",
                   "kept input");
        scene.click("handoffLeave");
        CHECK(!scene.app().completion()->outcome().isEmpty());
        checkOffline(scene);
    }
    CHECK(repo.snapshot() == launched);
    // The retried input is what a new window restores.
    Scene reopened(sandbox, repo);
    clickFile(reopened, "kept.txt");
    expectText(reopened.text("resultText"), "kept by hand\n!", "restored input");
    checkOffline(reopened);
}
} // namespace

void addWindowJourneys(JourneySuite &suite)
{
    const auto add = [&](const char *name, JourneySuite::Journey journey) {
        suite.add("window", name, std::move(journey));
    };
    add("outside-app placeholders retain literal stress text and reachable diagnostics",
        outsideAppPlaceholdersKeepStressText);
    add("a 1280x720 logical screen fits the window with every control and text pane reachable",
        logicalScreenFitsTheWindow);
    add("selecting a file shows its first undecided region in all three panes", selectionShowsTheFirstUndecidedRegion);
    add("a choice clears hand edits on a line deleted in the editor and on text an edit left unchanged",
        choiceClearsHandEdits);
    add("lines from both sides of one multi-line conflict keep their text, decisions and undo",
        linesFromBothSidesKeepTheirDecisions);
    add("closing with input recovery could not keep asks to retry, and cancel keeps the window open",
        closeGuardAsksToRetry);
}

} // namespace journey
