// The comparison view suites' scene: the real Main scene over one session, and probes for what its
// panes, gutters, bands, folds and cursor show. Every comparison suite file includes this.
#pragma once

#include "TestHarness.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/Translations.h"
#include "core/ResourceLimits.h"
#include "fixtures/AppSession.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/GuiState.h"
#include "fixtures/QuickScene.h"
#include "fixtures/SyncFaults.h"
#include "fixtures/WarningCapture.h"
#include "recovery/RecoveryRestore.h"
#include "storage/TextFormat.h"
#include "ui/TextBlocks.h"
#include <QAbstractTextDocumentLayout>
#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMap>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QtQml/qqml.h>
#include <QtQuickTest/quicktest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace comparison_scene {
using namespace branch_truce;
using test_support::markerKind;
using test_support::paintedMarkers;
using test_support::settle;
using test_support::settleFor;
using test_support::visualLines;

// A network access manager factory whose managers record every request and fail it, so a case can
// check that the scene asked for nothing. Managers may live on the image loader's thread.
class RecordingNetwork final : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager *create(QObject *parent) override { return new Manager(*this, parent); }
    [[nodiscard]] QList<QUrl> requests() const
    {
        const QMutexLocker lock(&mutex_);
        return requests_;
    }

private:
    class Manager final : public QNetworkAccessManager {
    public:
        Manager(RecordingNetwork &log, QObject *parent) : QNetworkAccessManager(parent), log_(log) {}

    protected:
        QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *data) override
        {
            log_.record(request.url());
            // A scheme no manager serves, so the request fails without leaving the process.
            return QNetworkAccessManager::createRequest(operation, QNetworkRequest(QUrl(QStringLiteral("refused:"))),
                                                        data);
        }

    private:
        RecordingNetwork &log_;
    };
    void record(const QUrl &url)
    {
        const QMutexLocker lock(&mutex_);
        requests_.append(url);
    }
    mutable QMutex mutex_;
    QList<QUrl> requests_;
};

class Fixture {
public:
    explicit Fixture(FileSession file = test_support::batchFile())
    {
        CHECK(controller.store()->appendFile(std::move(file)));
        CHECK(controller.selection()->select("choices"));
        engine.setNetworkAccessManagerFactory(&network);
        engine.setInitialProperties(controller.windowProperties());
        engine.loadFromModule("BranchTruce", "Main");
        CHECK(engine.rootObjects().size() == 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        CHECK(window);
        window->resize(1500, 1100);
        window->show();
        settle([&] { return item("resultText")->width() > 0 && item("resultText")->height() > 0; });
    }
    QQuickItem *item(const char *name) const
    {
        auto *found = test_support::visibleItem(window->contentItem(), QString::fromLatin1(name));
        if (!found) found = window->findChild<QQuickItem *>(QString::fromLatin1(name));
        // A toolbar creates its buttons outside the QObject tree, so look in the item tree too.
        if (!found) found = test_support::visualItem(window->contentItem(), QString::fromLatin1(name));
        if (!found) throw std::runtime_error(std::string("missing comparison item: ") + name);
        return found;
    }
    QObject *object(const char *name) const
    {
        auto *found = window->findChild<QObject *>(QString::fromLatin1(name));
        CHECK(found);
        return found;
    }
    const FileSession &file() { return *controller.store()->file("choices"); }
    QString text() const { return item("resultText")->property("text").toString(); }
    void select(int begin, int end)
    {
        CHECK(QMetaObject::invokeMethod(item("resultText"), "select", Q_ARG(int, begin), Q_ARG(int, end)));
    }
    void key(int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {})
    {
        auto *editor = item("resultText");
        editor->forceActiveFocus();
        QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
        QCoreApplication::sendEvent(editor, &event);
    }
    void click(const char *name)
    {
        auto *target = test_support::fileOverflowControl(name)
            ? test_support::fileToolbarControl(window, name)
            : test_support::waitForStable(
                  window, name,
                  [&] {
                      auto *candidate = item(name);
                      return test_support::reachable(window, candidate) ? candidate : nullptr;
                  },
                  60);
        if (!target->isVisible() || !target->isEnabled() || target->width() <= 0 || target->height() <= 0)
            throw std::runtime_error(std::string("unavailable control: ") + name);
        const auto center = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    bool toolbarEnabled(const char *name)
    {
        auto *button =
            test_support::fileOverflowControl(name) ? test_support::fileToolbarControl(window, name) : item(name);
        const bool enabled = button->isEnabled();
        if (test_support::fileOverflowControl(name)) test_support::closeFileToolbarMenu(window);
        return enabled;
    }
    // Click a control that has just appeared in a layout once the window has laid it out. The
    // control's own geometry can be final while its column has not yet moved the siblings after
    // it: under the KDE style the file toolbar still lay over the resolution confirmation, so
    // the click saved a draft instead.
    void clickLaidOut(const char *name)
    {
        auto *target = item(name);
        test_support::settleFor(std::string("layout of ") + name,
                                [&] { return target->isVisible() && target->width() > 0 && target->height() > 0; });
        if (!QQuickTest::qWaitForPolish(window, 5000))
            throw std::runtime_error(std::string("the window kept polishing before clicking ") + name);
        click(name);
    }
    // Open a toolbar menu and click one of its entries once the menu has laid out, then wait
    // until the menu has finished closing. The KDE style fades a menu out; reopening it during
    // that fade lets the end of the fade close the reopened menu.
    void clickMenuEntry(const char *menu, const char *entry)
    {
        click(menu);
        const auto name = QString::fromLatin1(entry);
        test_support::clickItem(window, test_support::waitForLayout(window, name, 60));
        test_support::settleFor(std::string("the menu holding ") + entry + " to close",
                                [&] { return !test_support::visualItem(window->contentItem(), name); });
    }
    void checkRecovered(const Draft &expected, std::size_t expectedFiles = 1)
    {
        const auto replay = replayRecovery(storage::RecoveryStore(controller.store()->recoveryDirectory()));
        if (!replay.succeeded) throw std::runtime_error("comparison recovery: " + replay.error.toStdString());
        CHECK(replay.succeeded && replay.files.size() == expectedFiles);
        const auto recovered =
            std::find_if(replay.files.begin(), replay.files.end(),
                         [&](const FileSession &candidate) { return candidate.id() == file().id(); });
        CHECK(recovered != replay.files.end());
        CHECK(recovered->current().draft == expected);
        CHECK(recovered->current().id == file().current().id);
        CHECK(*recovered->snapshots() == *file().snapshots());
        CHECK(!recovered->facts().resolved && !controller.files()->hasPendingRecovery());
    }

    // The recovery store syncs through this, so a case can fail one sync.
    std::shared_ptr<test_support::SyncFaults> faults = std::make_shared<test_support::SyncFaults>();
    Application controller{[&] {
        auto config = test_support::windowConfig();
        config.files = faults;
        return config;
    }()};
    // Every network request the scene makes; it outlives the engine that uses it.
    RecordingNetwork network;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

// A supported choice file under another identity, so one session holds several.
inline FileSession namedChoiceFile(const std::string &id)
{
    auto sources = *test_support::choiceFile().snapshots();
    const auto path = id + ".txt";
    sources.path = path;
    const auto text = sources.launchResult.text.utf8;
    return FileSession(id, path, {text, {}, 0}, {.snapshots = std::move(sources), .initializeChoices = true});
}

inline void clickItem(Fixture &fixture, QQuickItem *target) { test_support::clickItem(fixture.window, target); }

inline QQuickItem *waitForLayout(Fixture &fixture, const QString &name)
{
    return test_support::waitForLayout(fixture.window, name);
}

struct WorkSnapshot {
    RevisionId id = 0;
    Draft draft;
    bool canUndo = false;
    bool canRedo = false;
    QVariant progress;
    std::size_t undecided = 0;
    bool unsaved = false;
    bool resolved = false;
    bool needsReview = false;
    FileStatus status = FileStatus::Undecided;
    QString resultText;
    QString leftText;
    QString rightText;
};

inline WorkSnapshot captureWork(Fixture &fixture)
{
    const auto &current = fixture.file().current();
    const auto facts = fixture.file().facts();
    return {current.id,
            current.draft,
            fixture.file().canUndo(),
            fixture.file().canRedo(),
            QVariant(fixture.controller.selectedFile()->progress()),
            facts.undecided,
            facts.unsaved,
            facts.resolved,
            facts.needsReview,
            facts.status,
            fixture.item("resultText")->property("text").toString(),
            fixture.item("stage2Text")->property("text").toString(),
            fixture.item("stage3Text")->property("text").toString()};
}

inline void checkUnchanged(Fixture &fixture, const WorkSnapshot &before)
{
    const auto after = captureWork(fixture);
    CHECK(after.id == before.id);
    CHECK(after.draft == before.draft);
    CHECK(after.canUndo == before.canUndo && after.canRedo == before.canRedo);
    CHECK(after.progress == before.progress);
    CHECK(after.undecided == before.undecided && after.unsaved == before.unsaved && after.resolved == before.resolved
          && after.needsReview == before.needsReview && after.status == before.status);
    CHECK(after.resultText == before.resultText && after.leftText == before.leftText
          && after.rightText == before.rightText);
}

inline void expectDiagnostic(Fixture &fixture, const QString &diagnostic)
{
    auto *button = test_support::waitForStable(
        fixture.window, "Technical details",
        [&] {
            return test_support::reachableButton(fixture.window, fixture.window->contentItem(), "Technical details");
        },
        60);
    test_support::clickItem(fixture.window, button);
    settleFor("captured file diagnostic", [&] { return fixture.item("selectedProblemDiagnostic")->isVisible(); });
    auto *text = fixture.item("selectedProblemDiagnostic");
    CHECK(text->property("text").toString() == diagnostic);
    CHECK(text->property("readOnly").toBool() && text->property("selectByMouse").toBool());
    CHECK(text->property("textFormat").toInt() == 0); // TextEdit.PlainText
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    settleFor("file diagnostics closed",
              [&] { return !fixture.object("selectedProblemDetails")->property("visible").toBool(); });
}

inline void expectProgress(Fixture &fixture, bool saved, bool reviewed, bool resolved, bool earlier,
                           const QString &current, const QString &savedRevision, const QString &reviewedRevision,
                           const QString &stagedRevision, qulonglong undecided, qulonglong conflicts = 0)
{
    CHECK(fixture.controller.selection()->fileId() == QStringLiteral("choices"));
    const auto progress = fixture.controller.selectedFile()->progress();
    const auto describe = [&] {
        return std::string("progress saved=") + std::to_string(progress.value(QStringLiteral("saved")).toBool())
            + " reviewed=" + std::to_string(progress.value(QStringLiteral("reviewed")).toBool())
            + " resolved=" + std::to_string(progress.value(QStringLiteral("resolved")).toBool())
            + " earlier=" + std::to_string(progress.value(QStringLiteral("earlierResolved")).toBool())
            + " current=" + progress.value(QStringLiteral("currentRevision")).toString().toStdString()
            + " savedRev=" + progress.value(QStringLiteral("savedRevision")).toString().toStdString()
            + " reviewedRev=" + progress.value(QStringLiteral("reviewedRevision")).toString().toStdString()
            + " stagedRev=" + progress.value(QStringLiteral("stagedRevision")).toString().toStdString()
            + " undecided=" + std::to_string(progress.value(QStringLiteral("undecided")).toULongLong());
    };
    if (progress.value(QStringLiteral("saved")).toBool() != saved
        || progress.value(QStringLiteral("reviewed")).toBool() != reviewed
        || progress.value(QStringLiteral("resolved")).toBool() != resolved
        || progress.value(QStringLiteral("earlierResolved")).toBool() != earlier
        || progress.value(QStringLiteral("currentRevision")).toString() != current
        || progress.value(QStringLiteral("savedRevision")).toString() != savedRevision
        || progress.value(QStringLiteral("reviewedRevision")).toString() != reviewedRevision
        || progress.value(QStringLiteral("stagedRevision")).toString() != stagedRevision
        || progress.value(QStringLiteral("undecided")).toULongLong() != undecided)
        throw std::runtime_error(describe());
    CHECK(progress.value(QStringLiteral("saved")).toBool() == saved);
    CHECK(progress.value(QStringLiteral("reviewed")).toBool() == reviewed);
    CHECK(progress.value(QStringLiteral("resolved")).toBool() == resolved);
    CHECK(progress.value(QStringLiteral("earlierResolved")).toBool() == earlier);
    CHECK(progress.value(QStringLiteral("currentRevision")).toString() == current);
    CHECK(progress.value(QStringLiteral("savedRevision")).toString() == savedRevision);
    CHECK(progress.value(QStringLiteral("reviewedRevision")).toString() == reviewedRevision);
    CHECK(progress.value(QStringLiteral("stagedRevision")).toString() == stagedRevision);
    CHECK(progress.value(QStringLiteral("undecided")).toULongLong() == undecided);
    CHECK(progress.value(QStringLiteral("conflicts")).toULongLong() == conflicts);
}

inline void expectNavigation(Fixture &fixture, const QString &mode, const QString &fileId, const QString &scope,
                             int index, int count, int begin, int end, const QString &label,
                             const QString &contribution = {})
{
    const auto target = fixture.controller.navigator()->target();
    const auto actual = target.value(QStringLiteral("label")).toString();
    if (actual != label) throw std::runtime_error(("navigation label: " + actual).toStdString());
    CHECK(actual == label);
    CHECK(fixture.item("navigationTarget")->property("text").toString() == label);
    CHECK(target.value(QStringLiteral("mode")).toString() == mode);
    CHECK(target.value(QStringLiteral("fileId")).toString() == fileId);
    CHECK(target.value(QStringLiteral("scope")).toString() == scope);
    CHECK(target.value(QStringLiteral("index")).toInt() == index);
    CHECK(target.value(QStringLiteral("count")).toInt() == count);
    CHECK(fixture.controller.selection()->fileId() == fileId);
    if (scope != QStringLiteral("result")) return;
    CHECK(target.value(QStringLiteral("utf16Begin")).toInt() == begin);
    CHECK(target.value(QStringLiteral("utf16End")).toInt() == end);
    CHECK(target.value(QStringLiteral("byteBegin")).toULongLong() == static_cast<qulonglong>(begin));
    CHECK(target.value(QStringLiteral("byteEnd")).toULongLong() == static_cast<qulonglong>(end));
    if (!contribution.isEmpty()) CHECK(target.value(QStringLiteral("contributionId")).toString() == contribution);
    auto *editor = fixture.item("resultText");
    const int selectionStart = editor->property("selectionStart").toInt();
    const int selectionEnd = editor->property("selectionEnd").toInt();
    if (selectionStart != begin || selectionEnd != end)
        throw std::runtime_error("selection " + std::to_string(selectionStart) + "-" + std::to_string(selectionEnd));
    CHECK(selectionStart == begin && selectionEnd == end);
    CHECK(editor->property("selectedText").toString() == fixture.text().mid(begin, end - begin));
}

// count lines "<prefix> <n>", numbered from first.
inline std::string numberedLines(const std::string &prefix, int first, int count)
{
    std::string lines;
    for (int line = first; line < first + count; ++line) lines += prefix + ' ' + std::to_string(line) + '\n';
    return lines;
}

inline qreal paneLineHeight(QQuickItem *editor)
{
    auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    settle([&] {
        return document->begin().blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight
            && document->lastBlock().blockFormat().lineHeightType() == QTextBlockFormat::LineDistanceHeight;
    });
    const auto lines = editor->property("lineCount").toInt();
    CHECK(lines > 0);
    return editor->property("contentHeight").toReal() / lines;
}

inline qreal contentY(QQuickItem *viewport) { return viewport->property("contentY").toReal(); }

// Waits until the panes keep their place, so the window has finished its
// layout and the view has placed the panes for the selected file.
inline void settlePanes(Fixture &fixture)
{
    for (const auto *name : {"stage2Viewport", "resultViewport", "stage3Viewport"})
        test_support::waitForLayout(fixture.window, QString::fromLatin1(name), 60);
}

// Turns the mouse wheel down over the named viewport, a notch at a time, until
// it has scrolled to at least target, and waits for each movement to end. A
// Flickable does not scroll for a wheel event stamped with the previous time.
inline void wheelTo(Fixture &fixture, const char *name, qreal target)
{
    static quint64 timestamp = 0;
    auto *viewport = test_support::waitForLayout(fixture.window, QString::fromLatin1(name), 60);
    for (int notch = 0; notch < 400 && contentY(viewport) < target; ++notch) {
        const auto point = viewport->mapToScene(QPointF(viewport->width() / 2, viewport->height() / 2));
        QWheelEvent wheel(point, fixture.window->mapToGlobal(point), QPoint(), QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        timestamp += 250;
        wheel.setTimestamp(timestamp);
        QCoreApplication::sendEvent(fixture.window, &wheel);
        settle([&] { return !viewport->property("moving").toBool(); });
    }
    CHECK(contentY(viewport) >= target);
}

// For the scroll tests: the right source and the result hold 30 lines after
// line 50 that the left source lacks.
inline FileSession insertionFile()
{
    const auto left = numberedLines("shared", 1, 400);
    const auto right =
        numberedLines("shared", 1, 50) + numberedLines("right only", 1, 30) + numberedLines("shared", 51, 350);
    return test_support::choiceFile(left, right, right);
}

// Opens insertionFile with the result scrolled to top and its cursor at the
// start of the line holding marker, with keyboard focus in the result.
inline void placeResultCursor(Fixture &fixture, const QString &marker, qreal top)
{
    auto *editor = fixture.item("resultText");
    auto *result = fixture.item("resultViewport");
    settle([&] { return editor->property("lineCount").toInt() > 400; });
    settlePanes(fixture);
    const auto offset = static_cast<int>(fixture.text().indexOf(marker));
    CHECK(offset >= 0);
    fixture.select(offset, offset);
    editor->forceActiveFocus();
    CHECK(result->setProperty("contentY", top));
    test_support::waitForFrames(fixture.window, "the result is scrolled to its start");
    CHECK(editor->property("cursorPosition").toInt() == offset && contentY(result) == top);
}

// Lets the replaced text lay out before reading the panes.
inline void settleReplacement(Fixture &fixture)
{
    test_support::waitForFrames(fixture.window, "the replaced text is laid out");
}

// A cursor rectangle within the result viewport.
inline bool cursorInView(Fixture &fixture)
{
    auto *editor = fixture.item("resultText");
    auto *result = fixture.item("resultViewport");
    const auto cursor = editor->property("cursorRectangle").toRectF();
    const auto top = editor->y() + cursor.top() - contentY(result);
    return top >= 0 && top + cursor.height() <= result->height();
}

// Evaluates a QML expression on item, such as one of its attached properties.
inline QVariant qmlValue(QQuickItem *item, const QString &expression)
{
    QQmlExpression evaluated(qmlContext(item), item, expression);
    const auto value = evaluated.evaluate();
    if (evaluated.hasError()) throw std::runtime_error(evaluated.error().toString().toStdString());
    return value;
}

// Where the editor draws the character at position, in scene coordinates.
inline QRectF sceneRectangle(QQuickItem *editor, int position)
{
    QRectF rect;
    CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position)));
    return {editor->mapToScene(rect.topLeft()), rect.size()};
}

// The line numbers a pane's gutter shows, each with its scene y.
inline QMap<int, qreal> gutterNumbers(QQuickItem *gutter)
{
    QMap<int, qreal> shown;
    for (auto *child : gutter->childItems()) {
        bool number = false;
        const auto value = child->property("text").toString().toInt(&number);
        if (number && child->isVisible()) shown.insert(value, child->mapToScene(QPointF()).y());
    }
    return shown;
}

// The first line in the pane's view whose number is missing or not on the
// line's first row, or empty when every line in view has its number there.
inline std::string gutterMisalignment(Fixture &fixture, const std::string &pane)
{
    auto *editor = fixture.item((pane + "Text").c_str());
    auto *viewport = fixture.item((pane + "Viewport").c_str());
    const auto shown = gutterNumbers(fixture.item((pane + "Gutter").c_str()));
    const auto top = viewport->mapToScene(QPointF()).y();
    const auto bottom = top + viewport->height();
    const auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    int inView = 0;
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const auto row = sceneRectangle(editor, block.position());
        if (row.bottom() <= top || row.top() >= bottom) continue;
        ++inView;
        const auto number = block.blockNumber() + 1;
        if (!shown.contains(number)) return pane + " shows no number " + std::to_string(number);
        if (std::abs(shown.value(number) - row.top()) > 0.5)
            return pane + " shows number " + std::to_string(number) + " at y " + std::to_string(shown.value(number))
                + " for a line at y " + std::to_string(row.top());
    }
    return inView > 0 ? std::string() : pane + " has no line in view";
}

// How far right of the start of the editor's line holding text the character at column is drawn.
inline qreal columnOffset(QQuickItem *editor, const QString &text, int column)
{
    const auto start = static_cast<int>(editor->property("text").toString().indexOf(text));
    CHECK(start >= 0);
    return sceneRectangle(editor, start + column).x() - sceneRectangle(editor, start).x();
}

// What one pane paints, keyed by line: the kind of each line background, the
// kind of each gutter glyph, in the result the provenance letter, and
// "ignored" where the whole text takes the ignored side's tint. A line with
// no mark is absent.
struct PaneMarks {
    QMap<int, QString> backgrounds;
    QMap<int, QString> glyphs;
    QMap<int, QString> letters;
    QMap<int, QString> tinted;
    bool operator==(const PaneMarks &) const = default;
};

// The nonempty lines of an editor whose every character is drawn in its
// Kirigami.Theme.disabledTextColor, as "ignored".
inline QMap<int, QString> tintedLines(QQuickItem *editor)
{
    const auto disabled = qmlValue(editor, QStringLiteral("Kirigami.Theme.disabledTextColor")).value<QColor>();
    CHECK(disabled.isValid());
    const auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    QMap<int, QString> lines;
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const int length = block.length() - 1;
        if (length <= 0) continue;
        QList<bool> covered(length, false);
        for (const auto &range : block.layout()->formats()) {
            const auto brush = range.format.foreground();
            if (brush.style() == Qt::NoBrush || brush.color() != disabled) continue;
            for (int at = std::max(range.start, 0); at < std::min(range.start + range.length, length); ++at)
                covered[at] = true;
        }
        if (!covered.contains(false)) lines.insert(block.blockNumber() + 1, QStringLiteral("ignored"));
    }
    return lines;
}

inline PaneMarks paintedMarks(Fixture &fixture, const std::string &pane)
{
    PaneMarks found;
    QSet<int> glyphLines;
    for (auto *fill : fixture.item((pane + "BackgroundsLines").c_str())->childItems()) {
        if (fill->isVisible() && fill->property("line").isValid())
            found.backgrounds.insert(fill->property("line").toInt(), fill->property("kind").toString());
    }
    for (auto *glyph : fixture.item((pane + "GutterMarkerSlot").c_str())->childItems()) {
        const auto kind = glyph->property("kind").toString();
        if (glyph->isVisible() && !kind.isEmpty()) {
            const auto line = glyph->property("line").toInt();
            CHECK(!glyphLines.contains(line));
            glyphLines.insert(line);
        }
        if (glyph->isVisible() && !kind.isEmpty() && kind != "removed")
            found.glyphs.insert(glyph->property("line").toInt(), kind);
    }
    // Provenance is inspectable on the real line-number accessibility node.
    // No permanent B/L/R/m text or second glyph column may reserve code width.
    for (auto *child : fixture.item((pane + "Gutter").c_str())->childItems()) {
        const auto text = child->property("text").toString();
        bool number = false;
        const auto line = text.toInt(&number);
        if (!child->isVisible() || text.isEmpty()) continue;
        CHECK(number);
        const auto description = qmlValue(child, QStringLiteral("Accessible.description")).toString();
        if (description.contains("Matches both captured sources"))
            found.letters.insert(line, "B");
        else if (description.contains("Matches the captured left source"))
            found.letters.insert(line, "L");
        else if (description.contains("Matches the captured right source"))
            found.letters.insert(line, "R");
        else if (description.contains("Edited by hand"))
            found.letters.insert(line, "m");
    }
    CHECK(
        !test_support::visualItem(fixture.window->contentItem(), QString::fromStdString(pane + "GutterDecisionSlot")));
    found.tinted = tintedLines(fixture.item((pane + "Text").c_str()));
    return found;
}

inline std::string describeMarks(const PaneMarks &marks)
{
    const auto list = [](const QMap<int, QString> &lines) {
        std::string shown;
        for (auto it = lines.begin(); it != lines.end(); ++it)
            shown += " " + std::to_string(it.key()) + "=" + it.value().toStdString();
        return shown;
    };
    return "backgrounds" + list(marks.backgrounds) + "; glyphs" + list(marks.glyphs) + "; letters" + list(marks.letters)
        + "; tinted" + list(marks.tinted);
}

inline void expectMarks(Fixture &fixture, const std::string &when, const std::string &pane, const PaneMarks &expected)
{
    test_support::waitUntil([&] { return paintedMarks(fixture, pane) == expected; },
                            [&] {
                                return when + ", " + pane + " paints " + describeMarks(paintedMarks(fixture, pane))
                                    + ", not " + describeMarks(expected);
                            });
    if (pane != "stage2" && pane != "stage3") return;
    // The remaining number/state gutter shares the full viewport's row fills,
    // including accepted rows and after history changes.
    for (const auto *suffix : {"GutterBackgroundsLines"}) {
        QMap<int, QQuickItem *> strips;
        for (auto *fill : fixture.item((pane + suffix).c_str())->childItems())
            if (fill->isVisible() && fill->property("line").isValid())
                strips.insert(fill->property("line").toInt(), fill);
        CHECK(strips.keys() == expected.backgrounds.keys());
        for (auto *fill : fixture.item((pane + "BackgroundsLines").c_str())->childItems()) {
            const auto line = fill->property("line").toInt();
            if (!fill->isVisible() || !strips.contains(line)) continue;
            auto *strip = strips.value(line);
            const auto codeRect = fill->mapRectToScene(fill->boundingRect());
            const auto stripRect = strip->mapRectToScene(strip->boundingRect());
            CHECK(std::abs(codeRect.top() - stripRect.top()) < 0.5);
            CHECK(std::abs(codeRect.bottom() - stripRect.bottom()) < 0.5);
            CHECK(strip->property("color") == fill->property("color"));
            CHECK(strip->property("kind") == fill->property("kind"));
        }
    }
}

// The marks of a pane whose regions on lines 2, 4 and 6 have the given kinds;
// each gets a glyph of its kind, and a background of its kind unless it is
// ignored, whose text is tinted instead. The other lines carry no mark.
inline PaneMarks regionMarks(const QString &second, const QString &fourth, const QString &sixth,
                             const QMap<int, QString> &letters = {})
{
    PaneMarks marks;
    marks.letters = letters;
    for (const auto &[line, kind] : {std::pair{2, second}, std::pair{4, fourth}, std::pair{6, sixth}}) {
        if (kind.isEmpty()) continue;
        marks.glyphs.insert(line, kind);
        (kind == "ignored" ? marks.tinted : marks.backgrounds).insert(line, kind);
    }
    return marks;
}

// A connector band as its handle draws it: its kind, the source and result
// lines it joins as [begin, end), and the scene y of the top and bottom of
// each end.
struct Band {
    QString kind;
    int sourceBegin = 0;
    int sourceEnd = 0;
    int resultBegin = 0;
    int resultEnd = 0;
    qreal sourceTop = 0;
    qreal sourceBottom = 0;
    qreal resultTop = 0;
    qreal resultBottom = 0;
};

inline std::vector<Band> paintedBands(Fixture &fixture, const std::string &side)
{
    std::vector<Band> bands;
    for (auto *band : fixture.item((side + "Bands").c_str())->childItems()) {
        if (!band->property("kind").isValid()) continue;
        const auto line = [&](const char *name) {
            return qmlValue(band, QStringLiteral("modelData.") + QLatin1String(name)).toInt();
        };
        const auto y = [&](const char *name) {
            return band->mapToScene(QPointF(0, band->property(name).toReal())).y();
        };
        bands.push_back({band->property("kind").toString(), line("sourceBegin"), line("sourceEnd"), line("resultBegin"),
                         line("resultEnd"), y("sourceTop"), y("sourceBottom"), y("resultTop"), y("resultBottom")});
    }
    std::sort(bands.begin(), bands.end(), [](const Band &a, const Band &b) { return a.sourceBegin < b.sourceBegin; });
    return bands;
}

// Each band as "kind source begin-end > result begin-end", such as "conflict 2-3>2-3".
inline std::vector<std::string> bandLines(Fixture &fixture, const std::string &side)
{
    std::vector<std::string> lines;
    for (const auto &band : paintedBands(fixture, side)) {
        lines.push_back(band.kind.toStdString() + " " + std::to_string(band.sourceBegin) + "-"
                        + std::to_string(band.sourceEnd) + ">" + std::to_string(band.resultBegin) + "-"
                        + std::to_string(band.resultEnd));
    }
    return lines;
}

// The scene top and bottom of the pane's laid-out lines [begin, end).
// Ignored lines have tinted text and no background fill, so their geometry
// must come from the gutter's rows rather than from a visible fill. Empty
// ranges instead use the gap rule actually painted at the boundary.
inline std::optional<std::pair<qreal, qreal>> paintedSpan(Fixture &fixture, const std::string &pane, int begin, int end)
{
    if (begin == end) {
        for (auto *fill : fixture.item((pane + "BackgroundsGaps").c_str())->childItems()) {
            if (fill->isVisible() && fill->property("line").toInt() == begin) {
                const auto top = fill->mapToScene(QPointF()).y();
                return std::pair{top, top + fill->height()};
            }
        }
        return std::nullopt;
    }
    auto *gutter = fixture.item((pane + "Gutter").c_str());
    const auto rowIndex = [&](int line) {
        return qmlValue(gutter, QStringLiteral("rows.findIndex(row => row.number === %1)").arg(line)).toInt();
    };
    const int first = rowIndex(begin), last = rowIndex(end - 1);
    if (first < 0 || last < 0) return std::nullopt;
    const auto rowValue = [&](int index, const char *field) {
        return qmlValue(gutter, QStringLiteral("rows[%1].%2").arg(index).arg(QLatin1String(field))).toReal();
    };
    const auto top = gutter->mapToScene(QPointF(0, rowValue(first, "top"))).y();
    const auto bottom = gutter->mapToScene(QPointF(0, rowValue(last, "top") + rowValue(last, "extent"))).y();
    return std::pair{top, bottom};
}

// Waits until one side's handle draws the expected bands, each with its ends
// on the lines that the source pane and the result paint for its passage.
inline void expectBands(Fixture &fixture, const std::string &when, const std::string &side,
                        const std::vector<std::string> &expected)
{
    const auto misplaced = [&]() -> std::string {
        if (bandLines(fixture, side) != expected) return "the wrong bands";
        for (const auto &band : paintedBands(fixture, side)) {
            const auto source = paintedSpan(fixture, side, band.sourceBegin, band.sourceEnd);
            const auto result = paintedSpan(fixture, "result", band.resultBegin, band.resultEnd);
            if (!source || !result) return "line geometry not ready";
            const auto off = [](qreal a, qreal b) { return std::abs(a - b) > 0.5; };
            if (off(band.sourceTop, source->first) || off(band.sourceBottom, source->second)
                || off(band.resultTop, result->first) || off(band.resultBottom, result->second))
                return "a band at " + std::to_string(band.sourceTop) + "-" + std::to_string(band.sourceBottom) + ">"
                    + std::to_string(band.resultTop) + "-" + std::to_string(band.resultBottom) + " over lines at "
                    + std::to_string(source->first) + "-" + std::to_string(source->second) + ">"
                    + std::to_string(result->first) + "-" + std::to_string(result->second);
        }
        return {};
    };
    test_support::waitUntil([&] { return misplaced().empty(); },
                            [&] {
                                std::string drawn;
                                for (const auto &line : bandLines(fixture, side)) drawn += " " + line;
                                return when + ", the " + side + " handle draws " + misplaced() + ":" + drawn;
                            });
}

// The result's document, whose blocks the view folds.
inline const QTextDocument &resultDocument(Fixture &fixture)
{
    return *fixture.item("resultText")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
}

// The 1-based numbers of the result lines the view hides.
inline QList<int> foldedLines(Fixture &fixture)
{
    QList<int> hidden;
    const auto &document = resultDocument(fixture);
    for (auto block = document.begin(); block.isValid(); block = block.next())
        if (!block.isVisible()) hidden.push_back(block.blockNumber() + 1);
    return hidden;
}

// The 1-based lines of the result's text that start with one of the given prefixes.
inline QList<int> linesStarting(Fixture &fixture, const QStringList &prefixes)
{
    QList<int> found;
    const auto lines = fixture.text().split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
        if (std::any_of(prefixes.begin(), prefixes.end(),
                        [&](const QString &prefix) { return lines[i].startsWith(prefix); }))
            found.push_back(i + 1);
    return found;
}

// The visible items of one part of a pane layer, by line, such as the "row"s of
// the result's folded rows layer.
inline QMap<int, QQuickItem *> foldParts(QQuickItem *layer)
{
    QMap<int, QQuickItem *> found;
    auto *rows = layer->findChild<QQuickItem *>(layer->objectName() + QStringLiteral("Rows"));
    if (!rows) throw std::runtime_error("missing folded rows");
    for (auto *child : rows->childItems())
        if (child->isVisible() && child->property("line").isValid())
            found.insert(child->property("line").toInt(), child);
    return found;
}

inline QList<int> numbers(int first, int end, const QList<int> &except = {})
{
    QList<int> list;
    for (int line = first; line < end; ++line)
        if (!except.contains(line)) list.push_back(line);
    return list;
}

inline QString listed(const QList<int> &lines)
{
    QStringList shown;
    for (const auto line : lines) shown << QString::number(line);
    return shown.join(QLatin1Char(' '));
}

// How a result with conflict markers looks: the lines it hides, the lines
// numbered, the lines painted and marked as conflicts with markers remaining,
// the folded rows, and the y distance from line 1 to the line holding
// "bridge", in lines.
struct MarkerLook {
    QList<int> hidden;
    QList<int> numbered;
    QList<int> marked;
    QList<int> rows;
    qreal rowsToBridge = 0;
    bool operator==(const MarkerLook &) const = default;
};

inline MarkerLook markerLook(Fixture &fixture)
{
    MarkerLook look;
    look.hidden = foldedLines(fixture);
    look.numbered = gutterNumbers(fixture.item("resultGutter")).keys();
    const auto marks = paintedMarks(fixture, "result");
    for (auto it = marks.backgrounds.begin(); it != marks.backgrounds.end(); ++it)
        if (it.value() == "markers" && marks.glyphs.value(it.key()) == "markers") look.marked.push_back(it.key());
    look.rows = foldParts(fixture.item("resultFoldedRows")).keys();
    auto *editor = fixture.item("resultText");
    const auto first = sceneRectangle(editor, 0);
    const auto bridge = sceneRectangle(editor, static_cast<int>(fixture.text().indexOf("bridge")));
    look.rowsToBridge =
        std::round((bridge.top() - first.top()) / fixture.item("comparisonView")->property("rowHeight").toReal() * 100)
        / 100;
    return look;
}

inline std::string describeLook(const MarkerLook &look)
{
    return ("hidden [" + listed(look.hidden) + "] numbered [" + listed(look.numbered) + "] marked ["
            + listed(look.marked) + "] rows [" + listed(look.rows) + "] bridge at row "
            + QString::number(look.rowsToBridge))
        .toStdString();
}

// markedFile() with both blocks folded: lines 2-8 and 10-14 show as rows on lines 2 and 10.
inline MarkerLook markedFolded()
{
    const auto hidden = numbers(3, 9) + numbers(11, 15);
    return {hidden, numbers(1, 17, hidden), {2, 10}, {2, 10}, 2};
}

inline void expectLook(Fixture &fixture, const std::string &when, const MarkerLook &expected)
{
    test_support::waitUntil([&] { return markerLook(fixture) == expected; },
                            [&] {
                                return when + ", the result shows " + describeLook(markerLook(fixture)) + ", not "
                                    + describeLook(expected);
                            });
}

// Waits for the result's cursor to come to rest at position.
inline void expectCursor(Fixture &fixture, const std::string &when, int position)
{
    auto *editor = fixture.item("resultText");
    const auto describe = [&] {
        return when + ": the cursor is at " + std::to_string(editor->property("cursorPosition").toInt()) + ", not "
            + std::to_string(position);
    };
    test_support::waitUntil([&] { return editor->property("cursorPosition").toInt() == position; }, describe);
    // At rest: nothing queued moves it on.
    test_support::waitForFrames(fixture.window, when);
    const int at = editor->property("cursorPosition").toInt();
    if (at != position)
        throw std::runtime_error(when + ": the cursor is at " + std::to_string(at) + ", not "
                                 + std::to_string(position));
}

// Which cases one run executes. The deletion-at-limit cases edit multi-megabyte buffers in the
// real editor, so ctest runs them as their own entry with a timeout sized for them. Display
// runs the viewport cases that a real display can draw differently, on any platform; ctest
// does not run it, because ctest has no display.
enum class SceneSet { All, Scenes, Boundary, Display };

// Forwards a case to the suite only when its selection includes it.
class Group {
public:
    Group(test_support::Suite &suite, bool selected) : suite_(suite), selected_(selected) {}
    template <typename Test> void run(const std::string &name, Test test)
    {
        if (selected_) suite_.run(name, std::move(test));
    }

private:
    test_support::Suite &suite_;
    bool selected_;
};

// The groups one case can belong to.
struct Groups {
    Group &scenes;
    Group &boundary;
    Group &display;
};

void addLayoutCases(Groups groups);
void addEditingCases(Groups groups);
void addTextCases(Groups groups);
void addMarkCases(Groups groups);
void addFoldingCases(Groups groups);
void addNavigationCases(Groups groups);
void addSessionCases(Groups groups);
void addBoundaryCases(Groups groups);
} // namespace comparison_scene
