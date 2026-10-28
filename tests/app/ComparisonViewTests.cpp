#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "app/RecoveryRestore.h"
#include "core/ResourceLimits.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/QuickScene.h"
#include "fixtures/RecoveryFaults.h"
#include <QAccessible>
#include <QApplication>
#include <algorithm>
#include <optional>
#include <QClipboard>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QHostAddress>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMap>
#include <QSet>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtQml/qqml.h>
#include <QtQuickTest/quicktest.h>
#include <tuple>

using namespace branch_truce;

namespace {
using test_support::markerKind;
using test_support::paintedMarkers;
using test_support::settle;
using test_support::settleFor;
using test_support::visualLines;

class Fixture {
public:
    explicit Fixture(FileSession file = test_support::batchFile())
    {
        CHECK(controller.files()->appendFile(std::move(file)));
        CHECK(controller.selectFile("choices"));
        engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)}});
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
        auto *found = window->findChild<QQuickItem *>(QString::fromLatin1(name));
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
    const FileSession &file() { return *controller.files()->file("choices"); }
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
        auto *target = item(name);
        CHECK(target->isVisible() && target->isEnabled() && target->width() > 0 && target->height() > 0);
        const auto center = target->mapToScene(QPointF(target->width() / 2.0, target->height() / 2.0)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    // Click a control that has just appeared in a layout once the window has laid it out. The
    // control's own geometry can be final while its column has not yet moved the siblings after
    // it: under the KDE style the file toolbar still lay over the resolution confirmation, so
    // the click saved a draft instead (TRUCE-0144).
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
    // that fade lets the end of the fade close the reopened menu (TRUCE-0144).
    void clickMenuEntry(const char *menu, const char *entry)
    {
        click(menu);
        const auto name = QString::fromLatin1(entry);
        test_support::clickItem(window, test_support::waitForLayout(window, name, 60));
        test_support::settleFor(std::string("the menu holding ") + entry + " to close",
            [&] { return !test_support::visualItem(window->contentItem(), name); });
    }
    void checkRecovered(const Draft &expected)
    {
        const auto replay = replayRecovery(storage::RecoveryStore(controller.files()->recoveryDirectory()));
        if (!replay.succeeded) throw std::runtime_error("comparison recovery: " + replay.error.toStdString());
        CHECK(replay.succeeded && replay.files.size() == 1);
        CHECK(replay.files.front().current().draft == expected);
        CHECK(replay.files.front().current().id == file().current().id);
        CHECK(*replay.files.front().snapshots() == *file().snapshots());
        CHECK(!replay.files.front().facts().resolved && !controller.files()->hasPendingRecovery());
    }

    ApplicationController controller{gitOnPath()};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

// A supported choice file under another identity, so one session holds several.
FileSession namedChoiceFile(const std::string &id)
{
    auto sources = *test_support::choiceFile().snapshots();
    const auto path = id + ".txt";
    sources.path = path;
    const auto text = sources.launchResult.text.utf8;
    return FileSession(id, path, {text, {}, 0}, Support::Supported, {}, std::move(sources), true);
}

void clickItem(Fixture &fixture, QQuickItem *target)
{
    test_support::clickItem(fixture.window, target);
}

QQuickItem *waitForLayout(Fixture &fixture, const QString &name)
{
    return test_support::waitForLayout(fixture.window, name);
}

// atLimit ends in a non-newline byte that is exactly at a resource limit. Qt
// reports Backspace and Delete with control characters in text(), as real
// keyboards do, so the editor must not predict them as insertions.
void deletionAtLimit(const std::string &atLimit, const QString &rejection)
{
    Fixture fixture(test_support::choiceFile("x\n", "x\n", atLimit));
    auto *editor = fixture.item("resultText");
    const auto before = fixture.file().current();
    const auto size = static_cast<int>(atLimit.size());
    CHECK(fixture.text().toStdString() == atLimit);
    CHECK(!editor->property("readOnly").toBool() && fixture.controller.commandMessage().isEmpty());

    fixture.select(1, 1);
    fixture.key(Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    CHECK(fixture.file().current().id == before.id && fixture.text().toStdString() == atLimit);
    CHECK(!editor->property("readOnly").toBool());
    CHECK(fixture.controller.commandMessage() == rejection);
    fixture.checkRecovered(before.draft);

    fixture.select(size, size);
    fixture.key(Qt::Key_Backspace, Qt::NoModifier, QStringLiteral("\b"));
    const auto backspaced = fixture.file().current();
    CHECK(backspaced.id == before.id + 1);
    CHECK(backspaced.draft.text == atLimit.substr(0, atLimit.size() - 1));
    CHECK(fixture.text().toStdString() == backspaced.draft.text);
    CHECK(fixture.controller.commandMessage().isEmpty());
    fixture.checkRecovered(backspaced.draft);

    fixture.select(size - 1, size - 1);
    fixture.key(Qt::Key_A, Qt::NoModifier, QString(QLatin1Char(atLimit.back())));
    const auto refilled = fixture.file().current();
    CHECK(refilled.id == backspaced.id + 1 && refilled.draft.text == atLimit);

    fixture.select(0, 0);
    fixture.key(Qt::Key_Delete, Qt::NoModifier, QStringLiteral("\x7f"));
    const auto deleted = fixture.file().current();
    CHECK(deleted.id == refilled.id + 1);
    CHECK(deleted.draft.text == atLimit.substr(1));
    CHECK(fixture.text().toStdString() == deleted.draft.text);
    CHECK(!editor->property("readOnly").toBool() && fixture.controller.commandMessage().isEmpty());
    fixture.checkRecovered(deleted.draft);
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

WorkSnapshot captureWork(Fixture &fixture)
{
    const auto &current = fixture.file().current();
    const auto facts = fixture.file().facts();
    return {current.id, current.draft, fixture.file().canUndo(), fixture.file().canRedo(),
        fixture.controller.selectedFile().value(QStringLiteral("progress")),
        facts.undecided, facts.unsaved, facts.resolved, facts.needsReview, facts.status,
        fixture.item("resultText")->property("text").toString(),
        fixture.item("stage2Text")->property("text").toString(),
        fixture.item("stage3Text")->property("text").toString()};
}

void checkUnchanged(Fixture &fixture, const WorkSnapshot &before)
{
    const auto after = captureWork(fixture);
    CHECK(after.id == before.id);
    CHECK(after.draft == before.draft);
    CHECK(after.canUndo == before.canUndo && after.canRedo == before.canRedo);
    CHECK(after.progress == before.progress);
    CHECK(after.undecided == before.undecided && after.unsaved == before.unsaved
        && after.resolved == before.resolved && after.needsReview == before.needsReview
        && after.status == before.status);
    CHECK(after.resultText == before.resultText && after.leftText == before.leftText
        && after.rightText == before.rightText);
}

void expectProgress(Fixture &fixture, bool saved, bool reviewed, bool resolved, bool earlier,
                    const QString &current, const QString &savedRevision,
                    const QString &reviewedRevision, const QString &stagedRevision,
                    qulonglong undecided)
{
    CHECK(fixture.controller.selectedFileId() == QStringLiteral("choices"));
    const auto progress = fixture.controller.selectedFile().value(QStringLiteral("progress")).toMap();
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
    QString summary = QStringLiteral("Undecided: %1").arg(undecided);
    summary += QStringLiteral(" · ");
    summary += saved ? QStringLiteral("Saved") : QStringLiteral("Not saved");
    summary += QStringLiteral(" · ");
    summary += reviewed ? QStringLiteral("Reviewed") : QStringLiteral("Not reviewed");
    summary += QStringLiteral(" · ");
    summary += resolved ? QStringLiteral("Resolved") : QStringLiteral("Not resolved");
    if (earlier) summary += QStringLiteral(" · Earlier result staged; current draft not applied");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto status = test_support::shownText(fixture.item("selectedStatus"));
    if (!status.contains(summary))
        throw std::runtime_error(("status: " + status + " summary: " + summary).toStdString());
    CHECK(status.contains(summary));
}

void expectNavigation(Fixture &fixture, const QString &mode, const QString &fileId,
                      const QString &scope, int index, int count, int begin, int end,
                      const QString &label, const QString &contribution = {})
{
    const auto target = fixture.controller.navigationTarget();
    const auto actual = target.value(QStringLiteral("label")).toString();
    if (actual != label) throw std::runtime_error(("navigation label: " + actual).toStdString());
    CHECK(actual == label);
    CHECK(fixture.item("navigationTarget")->property("text").toString() == label);
    CHECK(target.value(QStringLiteral("mode")).toString() == mode);
    CHECK(target.value(QStringLiteral("fileId")).toString() == fileId);
    CHECK(target.value(QStringLiteral("scope")).toString() == scope);
    CHECK(target.value(QStringLiteral("index")).toInt() == index);
    CHECK(target.value(QStringLiteral("count")).toInt() == count);
    CHECK(fixture.controller.selectedFileId() == fileId);
    if (scope != QStringLiteral("result")) return;
    CHECK(target.value(QStringLiteral("utf16Begin")).toInt() == begin);
    CHECK(target.value(QStringLiteral("utf16End")).toInt() == end);
    CHECK(target.value(QStringLiteral("byteBegin")).toULongLong() == static_cast<qulonglong>(begin));
    CHECK(target.value(QStringLiteral("byteEnd")).toULongLong() == static_cast<qulonglong>(end));
    if (!contribution.isEmpty())
        CHECK(target.value(QStringLiteral("contributionId")).toString() == contribution);
    auto *editor = fixture.item("resultText");
    const int selectionStart = editor->property("selectionStart").toInt();
    const int selectionEnd = editor->property("selectionEnd").toInt();
    if (selectionStart != begin || selectionEnd != end)
        throw std::runtime_error("selection " + std::to_string(selectionStart) + "-" + std::to_string(selectionEnd));
    CHECK(selectionStart == begin && selectionEnd == end);
    CHECK(editor->property("selectedText").toString() == fixture.text().mid(begin, end - begin));
}

// count lines "<prefix> <n>", numbered from first.
std::string numberedLines(const std::string &prefix, int first, int count)
{
    std::string lines;
    for (int line = first; line < first + count; ++line) lines += prefix + ' ' + std::to_string(line) + '\n';
    return lines;
}

qreal paneLineHeight(QQuickItem *editor)
{
    const auto lines = editor->property("lineCount").toInt();
    CHECK(lines > 0);
    return editor->property("contentHeight").toReal() / lines;
}

qreal contentY(QQuickItem *viewport)
{
    return viewport->property("contentY").toReal();
}

// Waits until the panes keep their place, so the window has finished its
// layout and the view has placed the panes for the selected file.
void settlePanes(Fixture &fixture)
{
    for (const auto *name : {"stage2Viewport", "resultViewport", "stage3Viewport"})
        test_support::waitForLayout(fixture.window, QString::fromLatin1(name), 60);
}

// Turns the mouse wheel down over the named viewport, a notch at a time, until
// it has scrolled to at least target, and waits for each movement to end. A
// Flickable does not scroll for a wheel event stamped with the previous time.
void wheelTo(Fixture &fixture, const char *name, qreal target)
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
FileSession insertionFile()
{
    const auto left = numberedLines("shared", 1, 400);
    const auto right = numberedLines("shared", 1, 50) + numberedLines("right only", 1, 30)
        + numberedLines("shared", 51, 350);
    return test_support::choiceFile(left, right, right);
}

// Opens insertionFile with the result scrolled to top and its cursor at the
// start of the line holding marker, with keyboard focus in the result.
void placeResultCursor(Fixture &fixture, const QString &marker, qreal top)
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
    for (int turn = 0; turn < 10; ++turn) QCoreApplication::processEvents();
    CHECK(editor->property("cursorPosition").toInt() == offset && contentY(result) == top);
}

// Lets the replaced text lay out before reading the panes.
void settleReplacement()
{
    for (int turn = 0; turn < 10; ++turn) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// A cursor rectangle within the result viewport.
bool cursorInView(Fixture &fixture)
{
    auto *editor = fixture.item("resultText");
    auto *result = fixture.item("resultViewport");
    const auto cursor = editor->property("cursorRectangle").toRectF();
    const auto top = editor->y() + cursor.top() - contentY(result);
    return top >= 0 && top + cursor.height() <= result->height();
}

// Evaluates a QML expression on item, such as one of its attached properties.
QVariant qmlValue(QQuickItem *item, const QString &expression)
{
    QQmlExpression evaluated(qmlContext(item), item, expression);
    const auto value = evaluated.evaluate();
    if (evaluated.hasError()) throw std::runtime_error(evaluated.error().toString().toStdString());
    return value;
}

// Where the editor draws the character at position, in scene coordinates.
QRectF sceneRectangle(QQuickItem *editor, int position)
{
    QRectF rect;
    CHECK(QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position)));
    return {editor->mapToScene(rect.topLeft()), rect.size()};
}

// The line numbers a pane's gutter shows, each with its scene y.
QMap<int, qreal> gutterNumbers(QQuickItem *gutter)
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
std::string gutterMisalignment(Fixture &fixture, const std::string &pane)
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
qreal columnOffset(QQuickItem *editor, const QString &text, int column)
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
QMap<int, QString> tintedLines(QQuickItem *editor)
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

PaneMarks paintedMarks(Fixture &fixture, const std::string &pane)
{
    PaneMarks found;
    for (auto *fill : fixture.item((pane + "Backgrounds").c_str())->childItems()) {
        if (fill->isVisible() && fill->property("part").toString() == "line")
            found.backgrounds.insert(fill->property("line").toInt(), fill->property("kind").toString());
    }
    for (auto *glyph : fixture.item((pane + "GutterMarkerSlot").c_str())->childItems()) {
        const auto kind = glyph->property("kind").toString();
        if (glyph->isVisible() && !kind.isEmpty() && kind != "removed")
            found.glyphs.insert(glyph->property("line").toInt(), kind);
    }
    // The gutter's other text is the line numbers.
    for (auto *child : fixture.item((pane + "Gutter").c_str())->childItems()) {
        const auto text = child->property("text").toString();
        bool number = false;
        text.toInt(&number);
        if (child->isVisible() && !text.isEmpty() && !number)
            found.letters.insert(qmlValue(child, QStringLiteral("modelData.number")).toInt(), text);
    }
    found.tinted = tintedLines(fixture.item((pane + "Text").c_str()));
    return found;
}

std::string describeMarks(const PaneMarks &marks)
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

void expectMarks(Fixture &fixture, const std::string &when, const std::string &pane, const PaneMarks &expected)
{
    QElapsedTimer deadline;
    deadline.start();
    while (paintedMarks(fixture, pane) != expected && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto painted = paintedMarks(fixture, pane);
    if (painted != expected)
        throw std::runtime_error(when + ", " + pane + " paints " + describeMarks(painted)
            + ", not " + describeMarks(expected));
}

// The marks of a pane whose regions on lines 2, 4 and 6 have the given kinds;
// each gets a glyph of its kind, and a background of its kind unless it is
// ignored, whose text is tinted instead. The other lines carry no mark.
PaneMarks regionMarks(const QString &second, const QString &fourth, const QString &sixth,
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

std::vector<Band> paintedBands(Fixture &fixture, const std::string &side)
{
    std::vector<Band> bands;
    for (auto *band : fixture.item((side + "Bands").c_str())->childItems()) {
        if (!band->property("kind").isValid()) continue;
        const auto line = [&](const char *name) {
            return qmlValue(band, QStringLiteral("modelData.") + QLatin1String(name)).toInt();
        };
        const auto y = [&](const char *name) { return band->mapToScene(QPointF(0, band->property(name).toReal())).y(); };
        bands.push_back({band->property("kind").toString(), line("sourceBegin"), line("sourceEnd"),
            line("resultBegin"), line("resultEnd"), y("sourceTop"), y("sourceBottom"), y("resultTop"), y("resultBottom")});
    }
    std::sort(bands.begin(), bands.end(), [](const Band &a, const Band &b) { return a.sourceBegin < b.sourceBegin; });
    return bands;
}

// Each band as "kind source begin-end > result begin-end", such as "conflict 2-3>2-3".
std::vector<std::string> bandLines(Fixture &fixture, const std::string &side)
{
    std::vector<std::string> lines;
    for (const auto &band : paintedBands(fixture, side)) {
        lines.push_back(band.kind.toStdString() + " " + std::to_string(band.sourceBegin) + "-"
            + std::to_string(band.sourceEnd) + ">" + std::to_string(band.resultBegin) + "-" + std::to_string(band.resultEnd));
    }
    return lines;
}

// The scene top and bottom of what a pane's backgrounds paint for lines
// [begin, end): their fills, or for an empty range the gap rule on its boundary.
std::pair<qreal, qreal> paintedSpan(Fixture &fixture, const std::string &pane, int begin, int end)
{
    std::optional<qreal> top;
    std::optional<qreal> bottom;
    for (auto *fill : fixture.item((pane + "Backgrounds").c_str())->childItems()) {
        if (!fill->isVisible() || fill->property("part").toString() != (begin == end ? "gap" : "line")) continue;
        const auto line = fill->property("line").toInt();
        const auto y = fill->mapToScene(QPointF()).y();
        if (line == begin) top = y;
        if (line == std::max(begin, end - 1)) bottom = y + fill->height();
    }
    if (!top || !bottom)
        throw std::runtime_error(pane + " paints nothing for lines " + std::to_string(begin) + "-" + std::to_string(end));
    return {*top, *bottom};
}

// Waits until one side's handle draws the expected bands, each with its ends
// on the lines that the source pane and the result paint for its passage.
void expectBands(Fixture &fixture, const std::string &when, const std::string &side,
    const std::vector<std::string> &expected)
{
    const auto misplaced = [&]() -> std::string {
        if (bandLines(fixture, side) != expected) return "the wrong bands";
        for (const auto &band : paintedBands(fixture, side)) {
            const auto source = paintedSpan(fixture, side, band.sourceBegin, band.sourceEnd);
            const auto result = paintedSpan(fixture, "result", band.resultBegin, band.resultEnd);
            const auto off = [](qreal a, qreal b) { return std::abs(a - b) > 0.5; };
            if (off(band.sourceTop, source.first) || off(band.sourceBottom, source.second)
                    || off(band.resultTop, result.first) || off(band.resultBottom, result.second))
                return "a band at " + std::to_string(band.sourceTop) + "-" + std::to_string(band.sourceBottom) + ">"
                    + std::to_string(band.resultTop) + "-" + std::to_string(band.resultBottom) + " over lines at "
                    + std::to_string(source.first) + "-" + std::to_string(source.second) + ">"
                    + std::to_string(result.first) + "-" + std::to_string(result.second);
        }
        return {};
    };
    QElapsedTimer deadline;
    deadline.start();
    while (!misplaced().empty() && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (const auto problem = misplaced(); !problem.empty()) {
        std::string drawn;
        for (const auto &line : bandLines(fixture, side)) drawn += " " + line;
        throw std::runtime_error(when + ", the " + side + " handle draws " + problem + ":" + drawn);
    }
}

// The result's document, whose blocks the view folds.
const QTextDocument &resultDocument(Fixture &fixture)
{
    return *fixture.item("resultText")->property("textDocument").value<QQuickTextDocument *>()->textDocument();
}

// The 1-based numbers of the result lines the view hides.
QList<int> foldedLines(Fixture &fixture)
{
    QList<int> hidden;
    const auto &document = resultDocument(fixture);
    for (auto block = document.begin(); block.isValid(); block = block.next())
        if (!block.isVisible()) hidden.push_back(block.blockNumber() + 1);
    return hidden;
}

// The 1-based lines of the result's text that start with one of the given prefixes.
QList<int> linesStarting(Fixture &fixture, const QStringList &prefixes)
{
    QList<int> found;
    const auto lines = fixture.text().split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i)
        if (std::any_of(prefixes.begin(), prefixes.end(), [&](const QString &prefix) { return lines[i].startsWith(prefix); }))
            found.push_back(i + 1);
    return found;
}

// The visible items of one part of a pane layer, by line: the result gutter's
// "chip"s or the backgrounds' "fold" rules.
QMap<int, QQuickItem *> foldParts(QQuickItem *layer, const QString &part)
{
    QMap<int, QQuickItem *> found;
    for (auto *child : layer->childItems())
        if (child->isVisible() && child->property("part").toString() == part) found.insert(child->property("line").toInt(), child);
    return found;
}

QList<int> numbers(int first, int end, const QList<int> &except = {})
{
    QList<int> list;
    for (int line = first; line < end; ++line)
        if (!except.contains(line)) list.push_back(line);
    return list;
}

QString listed(const QList<int> &lines)
{
    QStringList shown;
    for (const auto line : lines) shown << QString::number(line);
    return shown.join(QLatin1Char(' '));
}

// How the result of markedFile(true) looks: the lines it hides, the lines
// painted and marked as conflicts with markers remaining, the chips and the
// fold rules, and the y distance from line 1 to line 3, in lines.
struct MarkerLook {
    QList<int> hidden;
    QList<int> numbered;
    QList<int> marked;
    QList<int> chips;
    QList<int> rules;
    qreal rowsToThirdLine = 0;
    bool operator==(const MarkerLook &) const = default;
};

MarkerLook markerLook(Fixture &fixture)
{
    MarkerLook look;
    look.hidden = foldedLines(fixture);
    look.numbered = gutterNumbers(fixture.item("resultGutter")).keys();
    const auto marks = paintedMarks(fixture, "result");
    for (auto it = marks.backgrounds.begin(); it != marks.backgrounds.end(); ++it)
        if (it.value() == "markers" && marks.glyphs.value(it.key()) == "markers") look.marked.push_back(it.key());
    look.chips = foldParts(fixture.item("resultGutterMarkerSlot"), "chip").keys();
    look.rules = foldParts(fixture.item("resultBackgrounds"), "fold").keys();
    auto *editor = fixture.item("resultText");
    const auto first = sceneRectangle(editor, 0);
    const auto third = sceneRectangle(editor, static_cast<int>(fixture.text().indexOf("left first")));
    look.rowsToThirdLine = std::round((third.top() - first.top()) / first.height() * 100) / 100;
    return look;
}

std::string describeLook(const MarkerLook &look)
{
    return ("hidden [" + listed(look.hidden) + "] numbered [" + listed(look.numbered) + "] marked ["
        + listed(look.marked) + "] chips [" + listed(look.chips) + "] rules [" + listed(look.rules)
        + "] line 3 at row " + QString::number(look.rowsToThirdLine)).toStdString();
}

void expectLook(Fixture &fixture, const std::string &when, const MarkerLook &expected)
{
    QElapsedTimer deadline;
    deadline.start();
    while (markerLook(fixture) != expected && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto look = markerLook(fixture);
    if (look != expected)
        throw std::runtime_error(when + ", the result shows " + describeLook(look) + ", not " + describeLook(expected));
}

// Waits for the result's cursor to come to rest at position.
void expectCursor(Fixture &fixture, const std::string &when, int position)
{
    auto *editor = fixture.item("resultText");
    QElapsedTimer deadline;
    deadline.start();
    while (editor->property("cursorPosition").toInt() != position && deadline.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    for (int turn = 0; turn < 5; ++turn) QCoreApplication::processEvents();
    const int at = editor->property("cursorPosition").toInt();
    if (at != position)
        throw std::runtime_error(when + ": the cursor is at " + std::to_string(at) + ", not " + std::to_string(position));
}

// Which cases one run executes. The deletion-at-limit cases edit multi-megabyte buffers in the
// real editor, so ctest runs them as their own entry with a timeout sized for them. Display
// runs the viewport cases that a real display can draw differently, on any platform; ctest
// does not run it, because ctest has no display.
enum class Selection { All, Scenes, Boundary, Display };

// Forwards a case to the suite only when its selection includes it.
class Group {
public:
    Group(test_support::Suite &suite, bool selected) : suite_(suite), selected_(selected) {}
    template<typename Test>
    void run(const std::string &name, Test test)
    {
        if (selected_) suite_.run(name, std::move(test));
    }
private:
    test_support::Suite &suite_;
    bool selected_;
};
}

class ComparisonViewTests {
public:
    int run(Selection selection)
    {
        test_support::Suite results("ComparisonViewTests");
        Group suite(results, selection == Selection::All || selection == Selection::Scenes);
        Group boundary(results, selection == Selection::All || selection == Selection::Boundary);
        Group display(results, selection == Selection::Display);
        suite.run("TRUCE-0020 actual comparison keeps verified sources around the editable launch result", [] {
            const std::string launch = "manual launch UTF-8 \xc3\xa9 \xf0\x9f\x98\x80\n";
            Fixture fixture(test_support::choiceFile("left capture\n", "right capture\n", launch));
            const auto before = fixture.file().current();
            CHECK(fixture.text().toStdString() == launch);
            for (const auto side : {SourceSide::Stage2, SourceSide::Stage3}) {
                const auto *prefix = side == SourceSide::Stage2 ? "stage2" : "stage3";
                const auto &source = side == SourceSide::Stage2 ? fixture.file().snapshots()->stage2 : fixture.file().snapshots()->stage3;
                CHECK(fixture.item((std::string(prefix) + "Text").c_str())->property("readOnly").toBool());
                CHECK(fixture.item((std::string(prefix) + "Text").c_str())->property("text").toString().toStdString() == source->worktree.utf8);
                const auto label = fixture.item((std::string(prefix) + "Label").c_str())->property("text").toString();
                CHECK(label.startsWith(side == SourceSide::Stage2 ? "Left source · " : "Right source · "));
                CHECK(label.contains(QString::fromStdString(source->label)));
                CHECK(fixture.item((std::string(prefix) + "Identity").c_str())->property("text").toString().contains(QString::fromStdString(source->objectId).left(10)));
                const auto projected = fixture.controller.selectedFile().value(prefix).toMap();
                CHECK(projected.value("commitId").toString().toStdString() == *source->commitId);
            }
            CHECK(!fixture.item("resultText")->property("readOnly").toBool());
            CHECK(fixture.item("resultLabel")->property("text").toString().startsWith("Editable result · "));
            const auto x = [&](const char *name) { return fixture.item(name)->mapToScene({0, 0}).x(); };
            CHECK(x("stage2Text") < x("resultText") && x("resultText") < x("stage3Text"));
            CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
            fixture.checkRecovered(before.draft);
        });
        suite.run("TRUCE-0163 toolbar commands report a button role and only the toggles are checkable", [] {
            Fixture fixture;
            // One line per control, so a failure names every control that reports the wrong role.
            const auto describe = [&](const char *name) {
                auto *item = fixture.item(name);
                auto *iface = QAccessible::queryAccessibleInterface(item);
                return QStringLiteral("%1 checkable=%2 role=%3").arg(QLatin1String(name))
                    .arg(item->property("checkable").toBool())
                    .arg(iface ? static_cast<int>(iface->role()) : -1);
            };
            QStringList commands;
            QStringList expectedCommands;
            for (const auto *name : {"saveDraft", "markResolved", "discardUnsaved", "undoCommand", "redoCommand",
                     "copyResult", "pasteResult", "openHandoff", "previousDifference", "nextDifference",
                     "previousUndecided", "undecidedCounter", "nextUndecided"}) {
                commands << describe(name);
                expectedCommands << QStringLiteral("%1 checkable=0 role=%2").arg(QLatin1String(name)).arg(static_cast<int>(QAccessible::Button));
            }
            for (const auto *name : {"navigateMenu", "sourceChoicesMenu"}) {
                commands << describe(name);
                expectedCommands << QStringLiteral("%1 checkable=0 role=%2").arg(QLatin1String(name)).arg(static_cast<int>(QAccessible::ButtonMenu));
            }
            QStringList toggles;
            QStringList expectedToggles;
            for (const auto *name : {"toggleFileList", "showSourceChoices"}) {
                toggles << describe(name);
                expectedToggles << QStringLiteral("%1 checkable=1 role=%2").arg(QLatin1String(name)).arg(static_cast<int>(QAccessible::CheckBox));
            }
            if (commands != expectedCommands || toggles != expectedToggles)
                throw std::runtime_error(("commands:\n" + commands.join('\n') + "\ntoggles:\n" + toggles.join('\n')).toStdString());
            CHECK(commands == expectedCommands && toggles == expectedToggles);
        });
        suite.run("TRUCE-0020 resizing earlier context and collapsed choices preserve current sources and manual work", [] {
            Fixture fixture;
            CHECK(fixture.controller.editResult("choices", "1", 0, 0, "manual prefix\n"));
            const auto before = fixture.file().current();
            const auto left = fixture.item("stage2Text")->property("text");
            const auto right = fixture.item("stage3Text")->property("text");
            CHECK(!fixture.item("earlierText")->isVisible());
            CHECK(fixture.item("showEarlier")->property("enabled").toBool());
            CHECK(fixture.item("showEarlier")->setProperty("checked", true));
            settle([&] { return fixture.item("earlierText")->isVisible(); });
            CHECK(fixture.item("earlierText")->property("readOnly").toBool());
            CHECK(fixture.item("earlierText")->property("text").toString().toStdString() == fixture.file().snapshots()->earlier->worktree.utf8);
            // The source choices start shown in the gutters; collapse them for the resize below.
            CHECK(fixture.item("scopedChoices")->isVisible());
            CHECK(fixture.item("showSourceChoices")->setProperty("checked", false));
            settle([&] { return !fixture.item("scopedChoices")->isVisible(); });
            CHECK(fixture.item("showEarlier")->setProperty("checked", false));
            auto *pane = fixture.item("stage2Label")->parentItem()->parentItem();
            const auto width = pane->width();
            QQmlExpression resize(qmlContext(pane), pane,
                QStringLiteral("SplitView.preferredWidth = %1").arg(width + 80));
            resize.evaluate();
            if (resize.hasError()) throw std::runtime_error(resize.error().toString().toStdString());
            settle([&] { return pane->width() > width + 20; });
            fixture.window->resize(1700, 1150);
            settle([&] { return fixture.item("comparisonPanes")->width() > 1200; });
            CHECK(fixture.item("stage2Text")->property("text") == left && fixture.item("stage3Text")->property("text") == right);
            CHECK(!fixture.item("earlierText")->isVisible() && !fixture.item("scopedChoices")->isVisible());
            CHECK(fixture.text().toStdString() == before.draft.text);
            CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
            fixture.checkRecovered(before.draft);
        });
        suite.run("TRUCE-0020 unavailable earlier context stays optional and leaves both current sources visible", [] {
            Fixture fixture(test_support::choiceFile());
            const auto before = fixture.file().current();
            CHECK(!fixture.item("showEarlier")->property("enabled").toBool());
            CHECK(fixture.item("showEarlier")->property("text").toString().contains("unavailable"));
            CHECK(fixture.item("showEarlier")->setProperty("checked", true));
            CHECK(!fixture.item("earlierText")->isVisible());
            CHECK(fixture.item("stage2Text")->isVisible() && fixture.item("stage3Text")->isVisible());
            CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
        });
        suite.run("TRUCE-0020 native typing and Unicode selection replacement use command-owned undo redo and recovery", [] {
            Fixture fixture(test_support::choiceFile("left\n", "right\n", "launch \xf0\x9f\x98\x80\n"));
            const auto initial = fixture.file().current();
            auto *editor = fixture.item("resultText");
            auto *document = editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
            CHECK(!document->isUndoRedoEnabled());
            CHECK(editor->setProperty("cursorPosition", 0));
            fixture.key(Qt::Key_A, Qt::NoModifier, "a");
            const auto typed = fixture.file().current();
            CHECK(typed.id == initial.id + 1 && typed.draft.text == "alaunch \xf0\x9f\x98\x80\n");
            fixture.checkRecovered(typed.draft);
            fixture.select(8, 10);
            fixture.key(Qt::Key_E, Qt::NoModifier, QString::fromUtf8("\xc3\xa9"));
            const auto replaced = fixture.file().current();
            CHECK(replaced.id == typed.id + 1 && replaced.draft.text == "alaunch \xc3\xa9\n");
            CHECK(replaced.draft.decisions == initial.draft.decisions && fixture.text().toStdString() == replaced.draft.text);
            fixture.checkRecovered(replaced.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft == typed.draft && fixture.text().toStdString() == typed.draft.text);
            fixture.checkRecovered(typed.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft == initial.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(fixture.file().current().draft == typed.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(fixture.file().current().draft == replaced.draft && !document->isUndoAvailable());
            fixture.checkRecovered(replaced.draft);
        });
        suite.run("TRUCE-0020 clipboard menus and shortcuts preserve atomic history and recovery", [] {
            Fixture fixture(test_support::choiceFile("left\n", "right\n", "original\n"));
            const auto initial = fixture.file().current();
            CHECK(fixture.controller.resultClipboard("selectAll"));
            CHECK(fixture.controller.resultClipboard("copy"));
            CHECK(QGuiApplication::clipboard()->text() == "original\n" && fixture.file().current().id == initial.id);
            QGuiApplication::clipboard()->setText(QString::fromUtf8("paste\t\xf0\x9f\x98\x80\n"));
            CHECK(fixture.controller.resultClipboard("paste"));
            const auto pasted = fixture.file().current();
            CHECK(pasted.id == initial.id + 1 && pasted.draft.text == "paste\t\xf0\x9f\x98\x80\n");
            fixture.checkRecovered(pasted.draft);
            fixture.select(0, 5);
            fixture.key(Qt::Key_C, Qt::ControlModifier);
            CHECK(QGuiApplication::clipboard()->text() == "paste" && fixture.file().current().id == pasted.id);
            CHECK(fixture.controller.resultClipboard("cut"));
            const auto cut = fixture.file().current();
            CHECK(cut.id == pasted.id + 1 && cut.draft.text == "\t\xf0\x9f\x98\x80\n");
            fixture.checkRecovered(cut.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft == pasted.draft);
            fixture.select(0, 5);
            QGuiApplication::clipboard()->setText("keyboard paste");
            fixture.key(Qt::Key_V, Qt::ControlModifier);
            const auto keyboard = fixture.file().current();
            CHECK(keyboard.draft.text == "keyboard paste\t\xf0\x9f\x98\x80\n");
            fixture.checkRecovered(keyboard.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft == pasted.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(fixture.file().current().draft == keyboard.draft);
            fixture.checkRecovered(keyboard.draft);
            QGuiApplication::clipboard()->clear();
        });
        suite.run("TRUCE-0020 an input-method preedit stays pending and its commit is one recovered command", [] {
            Fixture fixture(test_support::choiceFile());
            const auto before = fixture.file().current();
            auto *editor = fixture.item("resultText");
            CHECK(editor->setProperty("cursorPosition", 0));
            QInputMethodEvent preedit(QString::fromUtf8("\xe3\x81\x8b"), {});
            QCoreApplication::sendEvent(editor, &preedit);
            CHECK(fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
            QInputMethodEvent commit;
            commit.setCommitString(QString::fromUtf8("\xe3\x81\x8b"));
            QCoreApplication::sendEvent(editor, &commit);
            const auto chosen = fixture.file().current();
            CHECK(chosen.id == before.id + 1 && chosen.draft.text == std::string("\xe3\x81\x8b") + before.draft.text);
            fixture.checkRecovered(chosen.draft);
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft == before.draft);
        });
        for (const auto &scope : {QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("both")}) {
            suite.run("TRUCE-0018 batch preview identifies its file and source scope without changing another file " + scope.toStdString(), [=] {
                Fixture fixture;
                auto captured = *test_support::batchFile().snapshots();
                captured.path = "other.txt";
                CHECK(fixture.controller.files()->appendFile(FileSession("other", captured.path,
                    {captured.launchResult.text.utf8, {}, 0}, Support::Supported, {}, captured, true)));
                const auto other = fixture.controller.files()->file("other")->current();
                CHECK(fixture.controller.previewNonConflicting("choices", "1", scope));
                const auto preview = fixture.controller.nonConflictingPreview();
                CHECK(preview.value("path").toString() == "choices.txt");
                CHECK(preview.value("scope").toString() == (scope == "left" ? "Left" : scope == "right" ? "Right" : "Left and Right"));
                CHECK(preview.value("changes").toList().size() == (scope == "both" ? 2 : 1));
                for (const auto &change : preview.value("changes").toList()) {
                    const auto label = change.toMap().value("scope").toString();
                    CHECK(label.startsWith(scope == "right" ? "Right" : "Left") || (scope == "both" && label.startsWith("Right")));
                }
                CHECK(fixture.controller.applyNonConflicting());
                CHECK(fixture.controller.files()->file("other")->current().id == other.id);
                CHECK(fixture.controller.files()->file("other")->current().draft == other.draft);
                const auto replay = replayRecovery(storage::RecoveryStore(fixture.controller.files()->recoveryDirectory()));
                CHECK(replay.succeeded && replay.files.size() == 2);
                for (const auto &file : replay.files)
                    CHECK(file.current().draft == fixture.controller.files()->file(file.id())->current().draft);
            });
        }
        for (const int operation : {0, 1, 2}) {
            suite.run("wave 9 real confirmation cancellation and stale application preserve the draft " + std::to_string(operation), [=] {
                Fixture fixture(operation == 1 ? test_support::batchFile() : test_support::choiceFile());
                const auto before = fixture.file().current();
                const auto target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
                const auto open = [&] {
                    if (operation == 0)
                        CHECK(QMetaObject::invokeMethod(fixture.window, "choose", Q_ARG(QString, QString::fromStdString(target.source.contributionId)),
                            Q_ARG(QString, QStringLiteral("right-first"))));
                    else
                        CHECK(QMetaObject::invokeMethod(fixture.window, operation == 1 ? "chooseNonConflicting" : "chooseWholeFile",
                            Q_ARG(QString, operation == 1 ? QStringLiteral("both") : QStringLiteral("right"))));
                };
                const auto apply = [&] { return operation == 0 ? fixture.controller.applyChoice(true)
                    : operation == 1 ? fixture.controller.applyNonConflicting() : fixture.controller.applyWholeFile(); };
                const auto *dialogName = operation == 0 ? "replacementDialog" : operation == 1 ? "nonConflictingDialog" : "wholeFileDialog";
                open();
                auto *dialog = fixture.object(dialogName);
                CHECK(dialog->property("visible").toBool());
                CHECK(QMetaObject::invokeMethod(dialog, "reject"));
                CHECK(fixture.controller.commandPreview().isEmpty() && fixture.controller.nonConflictingPreview().isEmpty()
                    && fixture.controller.wholeFilePreview().isEmpty());
                CHECK(!apply() && fixture.file().current().id == before.id && fixture.file().current().draft == before.draft);
                fixture.checkRecovered(before.draft);
                open();
                CHECK(fixture.controller.editResult("choices", QString::number(before.id), 0, 0, "newer manual edit\n"));
                const auto newer = fixture.file().current();
                CHECK(!apply() && fixture.file().current().id == newer.id && fixture.file().current().draft == newer.draft);
                fixture.checkRecovered(newer.draft);
            });
        }
        suite.run("TRUCE-0015 Save and discard controls keep the app baseline and retry a failed Save", [] {
            Fixture fixture;
            auto saved = fixture.file().current().draft;
            saved.text += "saved app draft\n";
            saved.decisions["kept"] = {DecisionValue::Accepted, 4};
            CHECK(fixture.controller.files()->replaceDraft("choices", fixture.file().current().id, saved) == Transition::Applied);
            settle([&] { return fixture.controller.selectedFile().value("unsaved").toBool(); });
            fixture.click("saveDraft");
            CHECK(fixture.controller.commandMessage() == "Draft saved in Branch Truce; repository file unchanged.");
            CHECK(test_support::shownText(fixture.item("selectedStatus"))
                  .contains("Draft saved in Branch Truce; repository file unchanged"));
            CHECK(test_support::shownText(fixture.item("selectedStatus")).contains("Manual Save checkpoint retained"));
            CHECK(fixture.file().checkpoint() && fixture.file().checkpoint()->draft == saved);
            CHECK(!fixture.file().facts().unsaved && !fixture.file().written() && !fixture.file().staged());
            const auto savedStore = storage::RecoveryStore(fixture.controller.files()->recoveryDirectory()).read();
            CHECK(savedStore.succeeded);
            CHECK(std::any_of(savedStore.transactions.cbegin(), savedStore.transactions.cend(), [](const auto &state) {
                return state.value("action") == "manual-save" && state.value("written").isNull() && state.value("staged").isNull();
            }));
            auto discarded = saved;
            discarded.text += "discarded edit\n";
            discarded.decisions["kept"] = {DecisionValue::Ignored, {}};
            CHECK(fixture.controller.files()->replaceDraft("choices", fixture.file().current().id, discarded) == Transition::Applied);
            settle([&] { return fixture.controller.selectedFile().value("unsaved").toBool(); });
            CHECK(fixture.file().checkpoint()->draft == saved);
            fixture.click("discardUnsaved");
            CHECK(fixture.file().current().draft == saved && fixture.file().current().draft != discarded);
            CHECK(fixture.file().current().draft.decisions.at("kept").value == DecisionValue::Accepted);
            CHECK(fixture.text().toStdString() == saved.text);
            CHECK(!fixture.file().facts().unsaved && !fixture.file().written() && !fixture.file().staged());
            const auto discardedStore = storage::RecoveryStore(fixture.controller.files()->recoveryDirectory()).read();
            CHECK(discardedStore.succeeded && discardedStore.transactions.back().value("action") == "discard-to-checkpoint");
            const auto replay = replayRecovery(storage::RecoveryStore(fixture.controller.files()->recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.front().current().draft == saved);
            CHECK(replay.files.front().current().draft != discarded && !replay.files.front().written()
                  && !replay.files.front().staged());

            CHECK(fixture.controller.editResult("choices", QString::number(fixture.file().current().id),
                static_cast<int>(QString::fromStdString(saved.text).size()),
                static_cast<int>(QString::fromStdString(saved.text).size()), "retry me\n"));
            const auto edited = fixture.file().current().draft;
            {
                recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
                fixture.click("saveDraft");
            }
            settle([&] { return fixture.controller.selectedFile().value("recoveryState").toString() == "save-failed"; });
            CHECK(fixture.file().current().draft == edited && fixture.file().facts().unsaved);
            CHECK(fixture.file().checkpoint()->draft == saved);
            CHECK(fixture.text().toStdString() == edited.text);
            CHECK(test_support::shownText(fixture.item("selectedStatus")).contains("Draft save failed"));
            CHECK(test_support::shownText(fixture.item("selectedStatus")).contains("Unsaved draft"));
            CHECK(fixture.controller.commandMessage() == "Draft save failed. The edited result is still available.");
            CHECK(fixture.item("retrySave")->isVisible());
            CHECK(!fixture.item("saveDraft")->isEnabled() && !fixture.item("discardUnsaved")->isEnabled());
            CHECK(fixture.item("undoCommand")->isVisible() && !fixture.item("undoCommand")->isEnabled());
            CHECK(!fixture.file().written() && !fixture.file().staged());
            fixture.click("retrySave");
            settle([&] { return !fixture.controller.selectedFile().value("unsaved").toBool(); });
            CHECK(fixture.file().checkpoint() && fixture.file().checkpoint()->draft == edited);
            CHECK(!fixture.file().facts().unsaved && !fixture.item("retrySave")->isVisible());
            CHECK(fixture.controller.selectedFile().value("status").toString()
                  == "Draft saved in Branch Truce; repository file unchanged");
            CHECK(!fixture.file().written() && !fixture.file().staged());
        });
        suite.run("TRUCE-0058 search scope switches the pane Find selects", [] {
            Fixture fixture;
            const auto before = captureWork(fixture);
            auto *scope = fixture.item("searchScope");
            auto *query = fixture.item("searchQuery");
            CHECK(scope->property("count").toInt() == 3);
            CHECK(scope->property("currentIndex").toInt() == 0);
            CHECK(scope->property("currentText").toString() == "Left source");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Search scope: Left source.");
            CHECK(query->setProperty("text", "left only"));
            fixture.click("findPassage");
            auto *left = fixture.item("stage2Text");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Passage in Left source.");
            CHECK(left->property("selectedText").toString() == "left only");
            CHECK(left->property("selectionStart").toInt() == left->property("text").toString().indexOf("left only"));
            CHECK(fixture.item("resultText")->property("selectedText").toString().isEmpty());
            CHECK(fixture.item("stage3Text")->property("selectedText").toString().isEmpty());
            checkUnchanged(fixture, before);

            CHECK(scope->setProperty("currentIndex", 2));
            CHECK(scope->property("currentText").toString() == "Right source");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Search scope: Right source.");
            fixture.click("findPassage");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "No passage in Right source.");
            CHECK(fixture.item("stage3Text")->property("selectedText").toString().isEmpty());
            CHECK(query->setProperty("text", "right only"));
            fixture.click("findPassage");
            auto *right = fixture.item("stage3Text");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Passage in Right source.");
            CHECK(right->property("selectedText").toString() == "right only");
            CHECK(right->property("selectionStart").toInt() == right->property("text").toString().indexOf("right only"));
            checkUnchanged(fixture, before);

            CHECK(scope->setProperty("currentIndex", 1));
            CHECK(scope->property("currentText").toString() == "Editable result");
            CHECK(query->setProperty("text", "base conflict"));
            fixture.click("findPassage");
            auto *result = fixture.item("resultText");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Passage in Editable result.");
            CHECK(result->property("selectedText").toString() == "base conflict");
            CHECK(result->property("selectionStart").toInt() == result->property("text").toString().indexOf("base conflict"));
            checkUnchanged(fixture, before);

            CHECK(fixture.item("showEarlier")->setProperty("checked", true));
            settle([&] { return fixture.item("earlierText")->isVisible() && scope->property("count").toInt() == 4; });
            CHECK(scope->property("currentText").toString() == "Editable result");
            CHECK(scope->setProperty("currentIndex", 3));
            CHECK(scope->property("currentText").toString() == "Earlier version");
            fixture.click("findPassage");
            auto *earlier = fixture.item("earlierText");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Passage in Earlier version.");
            CHECK(earlier->property("selectedText").toString() == "base conflict");
            CHECK(earlier->property("selectionStart").toInt() == earlier->property("text").toString().indexOf("base conflict"));
            checkUnchanged(fixture, before);
        });
        suite.run("TRUCE-0058 Find selects a passage and wraps to the earlier match", [] {
            const std::string result = "prefix REPEAT middle REPEAT suffix\n";
            Fixture fixture(test_support::choiceFile("left\n", "right\n", result));
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
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Passage in Editable result.");
            checkUnchanged(fixture, before);
            fixture.click("findPassage");
            CHECK(editor->property("selectionStart").toInt() == second);
            CHECK(editor->property("selectedText").toString() == "REPEAT");
            checkUnchanged(fixture, before);
            fixture.click("findPassage");
            CHECK(editor->property("selectionStart").toInt() == first);
            CHECK(editor->property("selectedText").toString() == "REPEAT");
            CHECK(fixture.item("searchStatus")->property("text").toString() == "Passage in Editable result.");
            checkUnchanged(fixture, before);
        });
        suite.run("TRUCE-0058 wrapping a long line changes its visual line count", [] {
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
        suite.run("TRUCE-0112 each line number stays on the first row of its line while wrapped and scrolled", [] {
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
                wrappedInView = wrappedInView || (row.top() >= top && row.top() < top + viewport->height()
                    && visualLines(editor, block.blockNumber()) > 1);
            }
            CHECK(wrappedInView);
            CHECK(fixture.item("wrapLines")->setProperty("checked", false));
            settle([&] { return visualLines(editor, 6) == 1; });
            aligned("unwrapped again");
        });
        suite.run("TRUCE-0112 tabs stop every four characters at the same column in every pane", [] {
            const std::string shared = "abcde\n\tone\nabc\ttwo\nabcde\tthree\n\t\tfour\n";
            Fixture fixture(test_support::choiceFile(shared + "left\n", shared + "right\n", shared + "right\n"));
            settlePanes(fixture);
            // The text after the tabs, its column in the line, and the tab stops it sits on.
            const std::array<std::tuple<QString, int, int>, 4> lines{{
                {"\tone", 1, 1}, {"abc\ttwo", 4, 1}, {"abcde\tthree", 6, 2}, {"\t\tfour", 2, 2}}};
            // Four characters as drawn in the result.
            const auto four = columnOffset(fixture.item("resultText"), QStringLiteral("abcde\n"), 4);
            CHECK(four > 0);
            for (const auto *name : {"stage2Text", "resultText", "stage3Text"}) {
                auto *editor = fixture.item(name);
                CHECK(std::abs(columnOffset(editor, QStringLiteral("abcde\n"), 4) - four) < 0.01);
                for (const auto &[text, column, stops] : lines) {
                    const auto offset = columnOffset(editor, text, column);
                    if (std::abs(offset - stops * four) > 0.5)
                        throw std::runtime_error(std::string(name) + ": text after the tab in " + text.toStdString()
                            + " starts at " + std::to_string(offset) + ", not " + std::to_string(stops * four));
                }
            }
        });
        suite.run("TRUCE-0112 code and line numbers use the theme's fixed-width font", [] {
            auto sources = *test_support::choiceFile().snapshots();
            const auto earlier = sources.stage2->worktree.utf8;
            sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
                test_support::choiceText(earlier), "earlier-commit", "Common earlier version"};
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
        suite.run("TRUCE-0113 every coloured line has a gutter glyph of its kind", [] {
            Fixture fixture(test_support::choiceFile("header\nleft first\nshared\ntail\n",
                "header\nright first\nshared\nright extra\ntail\n"));
            const QMap<QString, QString> glyphs{{"added", "+"}, {"changed", "~"}, {"conflict", "!"},
                {"manual", "m"}, {"accepted", QString(QChar(0x2713))}, {"ignored", QString(QChar(0x00d7))},
                {"removed", QString(QChar(0x2212))}};
            // The kinds of the line backgrounds and gap rules painted in a pane,
            // and how many of them lack a visible glyph of that kind on their line.
            struct Painted { QSet<QString> kinds; int unmarked = 0; };
            const auto painted = [&](const std::string &pane) {
                Painted found;
                QMap<QPair<int, QString>, QString> shown;
                for (auto *glyph : fixture.item((pane + "GutterMarkerSlot").c_str())->childItems()) {
                    if (glyph->isVisible() && glyph->property("kind").isValid())
                        shown.insert({glyph->property("line").toInt(), glyph->property("kind").toString()},
                            glyph->property("text").toString());
                }
                for (auto *fill : fixture.item((pane + "Backgrounds").c_str())->childItems()) {
                    const auto part = fill->property("part").toString();
                    if (!fill->isVisible() || (part != "line" && part != "gap")) continue;
                    const auto kind = fill->property("kind").toString();
                    found.kinds.insert(kind);
                    const auto glyph = shown.value({fill->property("line").toInt(), kind});
                    if (glyph.isEmpty() || glyph != glyphs.value(kind)) ++found.unmarked;
                }
                return found;
            };
            const auto expect = [&](const std::string &pane, const QSet<QString> &kinds) {
                settleFor(pane + " diff colours", [&] { return painted(pane).kinds == kinds; });
                CHECK(painted(pane).unmarked == 0);
            };
            // At launch every region is undecided; the right source's extra line is a gap in the result.
            expect("stage2", {"conflict"});
            expect("result", {"conflict", "removed"});
            expect("stage3", {"conflict"});
            // Taking the left side decides every region, and typing in an accepted line marks it manual.
            // The edited region shows its passage colours again; the right's extra line stays ignored.
            const auto revision = [&] { return QString::number(fixture.file().current().id); };
            CHECK(fixture.controller.previewWholeFile("choices", revision(), "left"));
            CHECK(fixture.controller.applyWholeFile());
            const auto at = static_cast<int>(fixture.text().indexOf("left first"));
            CHECK(fixture.controller.editResult("choices", revision(), at, at + 4, "LEFT"));
            expect("stage2", {"changed"});
            expect("result", {"manual", "removed"});
            // TRUCE-0190: the right's extra line is ignored. Its glyph stays, its text
            // takes the disabled colour, and it gets no background fill.
            expect("stage3", {"changed"});
            const auto ignoredTinted = [&] {
                const auto stage3 = paintedMarks(fixture, "stage3");
                const auto ignored = stage3.glyphs.keys(QStringLiteral("ignored"));
                return !ignored.isEmpty() && stage3.tinted.keys() == ignored
                    && std::none_of(ignored.begin(), ignored.end(), [&](int line) { return stage3.backgrounds.contains(line); });
            };
            settleFor("stage3 ignored tint", ignoredTinted);
            CHECK(ignoredTinted());
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
            // regions on lines 4 and 6 stay undecided and keep their conflict marks. A
            // decided side is marked accepted or ignored on its source lines.
            struct Choice {
                QString action;
                QString leftState;
                QString rightState;
                bool resultChanges;
                PaneMarks stage2;
                PaneMarks result;
                PaneMarks stage3;
            };
            const std::array<Choice, 3> choices{{
                // The left line now matches the result. The result line stays a
                // conflict while the right contribution is undecided.
                {"accept", "accepted", "undecided", true, regionMarks("accepted", "conflict", "conflict"),
                    regionMarks("conflict", "conflict", "conflict", leftLetters),
                    regionMarks("conflict", "conflict", "conflict")},
                // Ignore leaves the result alone, so the left line still differs from it.
                {"ignore", "ignored", "undecided", false, regionMarks("ignored", "conflict", "conflict"),
                    regionMarks("conflict", "conflict", "conflict", launchLetters),
                    regionMarks("conflict", "conflict", "conflict")},
                // Resolving using the left side decides both contributions, so the
                // result line is only a change against the right source.
                {"resolve", "accepted", "ignored", true, regionMarks("accepted", "conflict", "conflict"),
                    regionMarks("changed", "conflict", "conflict", leftLetters),
                    regionMarks("ignored", "conflict", "conflict")},
            }};
            for (const auto &choice : choices) {
                // regionMarks gives an ignored line a tinted text and no background
                // (TRUCE-0190), so undo also checks that the tint goes.
                suite.run("TRUCE-0174 a scoped " + choice.action.toStdString()
                    + " repaints all three panes and the provenance letters, and undo restores them", [choice, launchLetters] {
                    Fixture fixture;
                    settlePanes(fixture);
                    expectMarks(fixture, "at launch", "stage2", regionMarks("conflict", "conflict", "conflict"));
                    expectMarks(fixture, "at launch", "result", regionMarks("conflict", "conflict", "conflict", launchLetters));
                    expectMarks(fixture, "at launch", "stage3", regionMarks("conflict", "conflict", "conflict"));
                    const auto before = fixture.text();
                    const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
                    CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                        QString::fromStdString(target.source.contributionId), choice.action));
                    CHECK(fixture.controller.applyChoice(true));
                    CHECK((fixture.text() != before) == choice.resultChanges);
                    if (choice.resultChanges) CHECK(fixture.text().contains(QStringLiteral("\nleft only\n")));
                    // Both scopes of a region share its decisions, so they end in the same state.
                    int chosen = 0;
                    for (const auto &entry : fixture.controller.selectedFile().value("regions").toList()) {
                        const auto region = entry.toMap();
                        const bool left = region.value("side").toString() == "stage2";
                        const bool here = region.value("sourceLineBegin").toInt() == 2;
                        chosen += left && here;
                        const auto expected = !here ? QStringLiteral("undecided") : left ? choice.leftState : choice.rightState;
                        if (region.value("state").toString() != expected)
                            throw std::runtime_error(region.value("id").toString().toStdString() + " is "
                                + region.value("state").toString().toStdString() + ", not " + expected.toStdString());
                    }
                    CHECK(chosen == 2);
                    const auto when = "after " + choice.action.toStdString();
                    expectMarks(fixture, when, "stage2", choice.stage2);
                    expectMarks(fixture, when, "result", choice.result);
                    expectMarks(fixture, when, "stage3", choice.stage3);
                    // Undo returns every pane to the undecided look of launch.
                    CHECK(fixture.controller.undo("choices", QString::number(fixture.file().current().id)));
                    CHECK(fixture.text() == before);
                    const auto undone = "after undoing " + choice.action.toStdString();
                    expectMarks(fixture, undone, "stage2", regionMarks("conflict", "conflict", "conflict"));
                    expectMarks(fixture, undone, "result", regionMarks("conflict", "conflict", "conflict", launchLetters));
                    expectMarks(fixture, undone, "stage3", regionMarks("conflict", "conflict", "conflict"));
                });
            }
        }
        suite.run("TRUCE-0178 each non-equal passage gets a band from its source lines to its result lines", [] {
            // In the batch fixture the left source differs from the result on lines 2
            // and 6 and the right source on lines 4 and 6, and every region is undecided.
            Fixture fixture;
            settlePanes(fixture);
            const std::vector<std::string> left{"conflict 2-3>2-3", "conflict 6-7>6-7"};
            const std::vector<std::string> right{"conflict 4-5>4-5", "conflict 6-7>6-7"};
            expectBands(fixture, "at launch", "stage2", left);
            expectBands(fixture, "at launch", "stage3", right);
            // A scoped choice on the left region at line 2 redraws the bands with the
            // passages, and undo restores them. Accepting makes the left line equal to
            // the result, so its band goes, and the right line now differs from it.
            struct Choice {
                QString action;
                std::vector<std::string> left;
                std::vector<std::string> right;
            };
            const std::array<Choice, 2> choices{{
                {"accept", {"conflict 6-7>6-7"}, {"conflict 2-3>2-3", "conflict 4-5>4-5", "conflict 6-7>6-7"}},
                {"ignore", {"ignored 2-3>2-3", "conflict 6-7>6-7"}, right},
            }};
            for (const auto &choice : choices) {
                const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
                CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                    QString::fromStdString(target.source.contributionId), choice.action));
                CHECK(fixture.controller.applyChoice(true));
                const auto when = "after " + choice.action.toStdString();
                expectBands(fixture, when, "stage2", choice.left);
                expectBands(fixture, when, "stage3", choice.right);
                CHECK(fixture.controller.undo("choices", QString::number(fixture.file().current().id)));
                const auto undone = "after undoing " + choice.action.toStdString();
                expectBands(fixture, undone, "stage2", left);
                expectBands(fixture, undone, "stage3", right);
            }
        });
        suite.run("TRUCE-0178 band ends move with the scroll of either pane", [] {
            // The result holds 30 lines after line 50 that the left source lacks, so the
            // left handle draws one band, a wedge on the left source's line boundary.
            Fixture fixture(insertionFile());
            auto *left = fixture.item("stage2Viewport");
            auto *result = fixture.item("resultViewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
            settlePanes(fixture);
            const auto line = paneLineHeight(editor);
            wheelTo(fixture, "stage2Viewport", 40 * line);
            const std::vector<std::string> band{"conflict 51-51>51-81"};
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
        suite.run("TRUCE-0178 the band handles drag to resize the panes and stay out of the focus chain", [] {
            Fixture fixture;
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
            const auto leftWidth = left->width();
            const auto resultWidth = result->width();
            const auto rightWidth = right->width();
            const auto from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
            QTest::mousePress(fixture.window, Qt::LeftButton, Qt::NoModifier, from);
            for (int step = 1; step <= 10; ++step)
                QTest::mouseMove(fixture.window, QPoint(from.x() + 8 * step, from.y()));
            QTest::mouseRelease(fixture.window, Qt::LeftButton, Qt::NoModifier, QPoint(from.x() + 80, from.y()));
            settle([&] { return std::abs(left->width() - (leftWidth + 80)) < 2; });
            CHECK(std::abs(result->width() - resultWidth) < 1 && std::abs(right->width() - (rightWidth - 80)) < 2);
            expectBands(fixture, "after the drag", "stage2", {"conflict 2-3>2-3", "conflict 6-7>6-7"});
        });
        suite.run("TRUCE-0179 marker lines fold behind a conflict band, keep the buffer and show on the toggle", [] {
            // markedFile(true) writes both sections between markers with diff3 base sections:
            // lines 2-11 and 13-19, whose marker lines are 2, 5, 8, 11, 13, 15, 17 and 19.
            Fixture fixture(test_support::markedFile(true));
            settlePanes(fixture);
            const auto text = QString::fromStdString(test_support::markedText(true));
            const auto markerLines = linesStarting(fixture, {"<<<<<<<", "|||||||", "=======", ">>>>>>>"});
            CHECK(markerLines == QList<int>({2, 5, 8, 11, 13, 15, 17, 19}));
            const auto lineCount = resultDocument(fixture).blockCount();
            const auto blockLines = numbers(2, 12) + numbers(13, 20);
            // Folded: the marker lines take no row and get no number, the lines between them are
            // marked as conflicts with markers remaining, and a chip opens each block.
            const MarkerLook folded{markerLines, numbers(1, lineCount + 1, markerLines),
                numbers(2, 12, markerLines) + numbers(13, 20, markerLines), {3, 14}, {3, 6, 9, 12, 14, 16, 18, 20}, 1};
            expectLook(fixture, "at launch", folded);
            CHECK(fixture.text() == text && fixture.file().current().draft.text == text.toStdString());
            CHECK(resultDocument(fixture).toPlainText() == text);
            // The marked lines get the conflict colours and the chip says what is hidden. Each
            // layer's own theme gives the colours, as the backgrounds layer takes no input.
            const auto theme = [&](const char *layer, const char *role) {
                return qmlValue(fixture.item(layer), QStringLiteral("Kirigami.Theme.") + QLatin1String(role)).value<QColor>();
            };
            for (auto *fill : fixture.item("resultBackgrounds")->childItems()) {
                if (fill->isVisible() && fill->property("kind").toString() == "markers" && fill->property("part").toString() == "line")
                    CHECK(fill->property("color").value<QColor>() == theme("resultBackgrounds", "negativeBackgroundColor"));
            }
            for (auto *glyph : fixture.item("resultGutterMarkerSlot")->childItems()) {
                if (glyph->isVisible() && glyph->property("kind").toString() == "markers" && glyph->property("part").toString() != "chip") {
                    CHECK(glyph->property("text").toString() == "!");
                    CHECK(glyph->property("color").value<QColor>() == theme("resultGutterMarkerSlot", "negativeTextColor"));
                }
            }
            const auto chips = foldParts(fixture.item("resultGutterMarkerSlot"), "chip");
            for (auto *chip : chips) {
                CHECK(chip->property("text").toString() == "!");
                CHECK(chip->property("color").value<QColor>() == theme("resultGutterMarkerSlot", "negativeBackgroundColor"));
            }
            const auto described = qmlValue(chips.value(3), QStringLiteral("Accessible.description")).toString();
            if (described != "Conflict markers remain: 1 marker line hidden above")
                throw std::runtime_error("first chip described as " + described.toStdString());
            // Every band to the result meets a block, so it is a conflict band.
            for (const auto *side : {"stage2", "stage3"}) {
                const auto bands = bandLines(fixture, side);
                CHECK(!bands.empty());
                for (const auto &band : bands)
                    if (band.rfind("conflict ", 0) != 0) throw std::runtime_error(std::string(side) + " band " + band);
            }
            // The toggle shows the raw lines as conflicts and changes no work.
            const auto before = captureWork(fixture);
            auto *toggle = fixture.object("showConflictMarkers");
            CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
            CHECK(toggle->property("checked").toBool());
            expectLook(fixture, "with the markers shown", {{}, numbers(1, lineCount + 1), blockLines, {}, {}, 2});
            checkUnchanged(fixture, before);
            CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
            expectLook(fixture, "with the markers folded again", folded);
            checkUnchanged(fixture, before);
        });
        suite.run("TRUCE-0179 keys and selections step over folded marker lines", [] {
            // markedFile(): lines 2, 5 and 8 are the first block's markers.
            Fixture fixture(test_support::markedFile());
            settlePanes(fixture);
            expectLook(fixture, "at launch", {{2, 5, 8, 10, 12, 14}, numbers(1, 17, {2, 5, 8, 10, 12, 14}),
                {3, 4, 6, 7, 11, 13}, {3, 11}, {3, 6, 9, 11, 13, 15}, 1});
            const auto text = fixture.text();
            const auto at = [&](const char *line) { return static_cast<int>(text.indexOf(QLatin1String(line))); };
            auto *editor = fixture.item("resultText");
            const auto selection = [&] {
                return std::pair{editor->property("selectionStart").toInt(), editor->property("selectionEnd").toInt()};
            };
            // Down from column 3 of the header passes the folded line 2 and keeps the column.
            fixture.select(3, 3);
            expectCursor(fixture, "after placing the cursor", 3);
            fixture.key(Qt::Key_Down);
            expectCursor(fixture, "down from the header", at("left first") + 3);
            // Down and up over the folded separator keep the column; right and left cross it.
            fixture.select(at("left second") + 5, at("left second") + 5);
            fixture.key(Qt::Key_Down);
            expectCursor(fixture, "down over the separator", at("right first") + 5);
            fixture.key(Qt::Key_Up);
            expectCursor(fixture, "up over the separator", at("left second") + 5);
            fixture.select(at("=======") - 1, at("=======") - 1);
            fixture.key(Qt::Key_Right);
            expectCursor(fixture, "right over the separator", at("right first"));
            fixture.key(Qt::Key_Left);
            expectCursor(fixture, "left over the separator", at("=======") - 1);
            // Shift+Down extends a selection over the folded line and keeps its anchor.
            fixture.select(at("left second"), at("left second"));
            fixture.key(Qt::Key_Down, Qt::ShiftModifier);
            expectCursor(fixture, "shift+down over the separator", at("right first"));
            CHECK(selection() == std::pair(at("left second"), at("right first")));
            // A selection set by code that ends on a folded line ends before it instead.
            fixture.select(at("left first"), at("======="));
            expectCursor(fixture, "after selecting the left side", at("=======") - 1);
            CHECK(selection() == std::pair(at("left first"), at("=======") - 1));
            CHECK(fixture.text() == text);
        });
        suite.run("TRUCE-0179 a hand edit that touches a folded line shows it and undo folds it again", [] {
            Fixture fixture(test_support::markedFile());
            settlePanes(fixture);
            const MarkerLook folded{{2, 5, 8, 10, 12, 14}, numbers(1, 17, {2, 5, 8, 10, 12, 14}),
                {3, 4, 6, 7, 11, 13}, {3, 11}, {3, 6, 9, 11, 13, 15}, 1};
            expectLook(fixture, "at launch", folded);
            const auto launched = fixture.text();
            // Backspace at the start of "bridge" joins it to the folded end marker above it.
            const auto bridge = static_cast<int>(launched.indexOf("bridge"));
            fixture.select(bridge, bridge);
            fixture.key(Qt::Key_Backspace);
            const auto joined = QString(launched).replace(">>>>>>> theirs\nbridge", ">>>>>>> theirsbridge");
            CHECK(fixture.text() == joined && fixture.file().current().draft.text == joined.toStdString());
            // The first block no longer matches the one Git wrote, so all of its lines show;
            // the second block stays folded.
            const MarkerLook shown{{9, 11, 13}, numbers(1, 16, {9, 11, 13}), {2, 3, 4, 5, 6, 7, 8, 10, 12},
                {10}, {10, 12, 14}, 2};
            expectLook(fixture, "after joining a line to the end marker", shown);
            expectCursor(fixture, "after the join", bridge - 1);
            fixture.checkRecovered(fixture.file().current().draft);
            // Undo restores the text and its fold, and redo shows the edited block again.
            CHECK(fixture.controller.undo("choices", QString::number(fixture.file().current().id)));
            CHECK(fixture.text() == launched);
            expectLook(fixture, "after undo", folded);
            CHECK(fixture.controller.redo("choices", QString::number(fixture.file().current().id)));
            CHECK(fixture.text() == joined);
            expectLook(fixture, "after redo", shown);
            fixture.checkRecovered(fixture.file().current().draft);
        });
        suite.run("TRUCE-0179 finding text on a folded line or going to a blocking marker shows the markers", [] {
            Fixture fixture(test_support::markedFile());
            settlePanes(fixture);
            CHECK(foldedLines(fixture) == QList<int>({2, 5, 8, 10, 12, 14}));
            // Find in the result selects a match on a folded end marker and shows the markers.
            auto *toggle = fixture.object("showConflictMarkers");
            CHECK(fixture.item("searchScope")->setProperty("currentIndex", 1));
            CHECK(fixture.item("searchQuery")->setProperty("text", QStringLiteral("theirs")));
            fixture.click("findPassage");
            const auto theirs = static_cast<int>(fixture.text().indexOf("theirs"));
            expectCursor(fixture, "after finding theirs", theirs + 6);
            CHECK(fixture.item("resultText")->property("selectionStart").toInt() == theirs);
            CHECK(toggle->property("checked").toBool() && foldedLines(fixture).isEmpty());
            CHECK(QMetaObject::invokeMethod(toggle, "trigger"));
            settle([&] { return foldedLines(fixture) == QList<int>({2, 5, 8, 10, 12, 14}); });
            CHECK(!fixture.controller.markResolved("choices", QString::number(fixture.file().current().id), true));
            CHECK(fixture.controller.markerBlocker().value("line").toInt() == 2);
            const auto before = captureWork(fixture);
            CHECK(QMetaObject::invokeMethod(fixture.object("showFirstMarker"), "trigger"));
            // The cursor stays on the marker it went to, which the view now shows.
            expectCursor(fixture, "after going to the first marker", static_cast<int>(fixture.text().indexOf("<<<<<<< ours")));
            CHECK(toggle->property("checked").toBool() && foldedLines(fixture).isEmpty());
            checkUnchanged(fixture, before);
        });
        suite.run("TRUCE-0179 deciding a section removes its fold and markersRemain, and Save writes the buffer either way", [] {
            const auto remaining = [](Fixture &fixture) {
                QStringList found;
                for (const auto &entry : fixture.controller.selectedFile().value("regions").toList()) {
                    const auto region = entry.toMap();
                    found << region.value("side").toString() + ":" + region.value("section").toString() + "="
                        + (region.value("markersRemain").toBool() ? "remain" : "gone");
                }
                found.sort();
                return found;
            };
            // Save with the markers folded and with them shown writes the same bytes, the buffer's.
            std::optional<std::string> savedBytes;
            for (const bool shown : {false, true}) {
                Fixture fixture(test_support::markedFile());
                settlePanes(fixture);
                if (shown) CHECK(QMetaObject::invokeMethod(fixture.object("showConflictMarkers"), "trigger"));
                expectLook(fixture, shown ? "shown before Save" : "folded before Save", shown
                    ? MarkerLook{{}, numbers(1, 17), numbers(2, 9) + numbers(10, 15), {}, {}, 2}
                    : MarkerLook{{2, 5, 8, 10, 12, 14}, numbers(1, 17, {2, 5, 8, 10, 12, 14}),
                        {3, 4, 6, 7, 11, 13}, {3, 11}, {3, 6, 9, 11, 13, 15}, 1});
                const auto header = static_cast<int>(fixture.text().indexOf("header"));
                fixture.select(header, header);
                fixture.key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
                settle([&] { return fixture.controller.selectedFile().value("unsaved").toBool(); });
                fixture.click("saveDraft");
                CHECK(fixture.file().checkpoint());
                const auto replay = replayRecovery(storage::RecoveryStore(fixture.controller.files()->recoveryDirectory()));
                CHECK(replay.succeeded && replay.files.size() == 1 && replay.files.front().checkpoint());
                const auto bytes = replay.files.front().checkpoint()->draft.text;
                CHECK(bytes == "x" + test_support::markedText() && bytes == fixture.file().checkpoint()->draft.text);
                CHECK(QString::fromStdString(bytes) == fixture.text());
                if (savedBytes) CHECK(bytes == *savedBytes);
                savedBytes = bytes;
            }
            Fixture fixture(test_support::markedFile());
            settlePanes(fixture);
            const auto atLaunch = remaining(fixture);
            CHECK(atLaunch.size() >= 4 && atLaunch.filter("=gone").isEmpty());
            CHECK(fixture.controller.selectedFile().value("markerBlocks").toList().size() == 2);
            // Resolving the first section with the left side removes its markers, so only the
            // second block stays folded and only its regions still have markers.
            const auto &first = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
            const auto section = QString::fromStdString(first.section);
            CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                QString::fromStdString(first.source.contributionId), "resolve"));
            CHECK(fixture.controller.applyChoice(true));
            CHECK(!fixture.text().contains(QStringLiteral(">>>>>>> theirs\nbridge")));
            CHECK(fixture.controller.selectedFile().value("markerBlocks").toList().size() == 1);
            for (const auto &entry : remaining(fixture))
                CHECK(entry.endsWith(entry.contains(":" + section + "=") ? "=gone" : "=remain"));
            expectLook(fixture, "after resolving the first section", {{5, 7, 9}, numbers(1, 12, {5, 7, 9}),
                {6, 8}, {6}, {6, 8, 10}, 1});
            CHECK(fixture.controller.undo("choices", QString::number(fixture.file().current().id)));
            CHECK(remaining(fixture) == atLaunch);
            expectLook(fixture, "after undoing the choice", {{2, 5, 8, 10, 12, 14}, numbers(1, 17, {2, 5, 8, 10, 12, 14}),
                {3, 4, 6, 7, 11, 13}, {3, 11}, {3, 6, 9, 11, 13, 15}, 1});
        });
        suite.run("TRUCE-0192 a block around two sections split by a shared line folds until both are decided", [] {
            // splitFile writes one block, lines 2-10, around two sections split by "shared";
            // its marker lines are 2, 6 and 10.
            Fixture fixture(test_support::splitFile());
            settlePanes(fixture);
            CHECK(linesStarting(fixture, {"<<<<<<<", "=======", ">>>>>>>"}) == QList<int>({2, 6, 10}));
            // Whether each region still has markers, in the controller's order.
            const auto remain = [&] {
                QList<bool> found;
                for (const auto &entry : fixture.controller.selectedFile().value("regions").toList())
                    found.push_back(entry.toMap().value("markersRemain").toBool());
                return found;
            };
            const auto folded = [&] {
                const auto blocks = fixture.controller.selectedFile().value("markerBlocks").toList();
                return blocks.size() == 1 && blocks.front().toMap().value("folded").toBool();
            };
            const auto atLaunch = remain();
            CHECK(atLaunch.size() >= 4 && !atLaunch.contains(false) && folded());
            settle([&] { return foldedLines(fixture) == QList<int>({2, 6, 10}); });
            const auto resolve = [&](SourceSide side, std::size_t at) {
                const auto &target = test_support::choiceTarget(fixture.file(), side, ChoiceScope::Section, at);
                CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                    QString::fromStdString(target.source.contributionId), "resolve"));
                CHECK(fixture.controller.applyChoice(true));
            };
            // Deciding the first section keeps the block, its fold and every region's markers.
            resolve(SourceSide::Stage2, 0);
            CHECK(fixture.text() == QString::fromStdString(test_support::splitText()) && remain() == atLaunch && folded());
            settle([&] { return foldedLines(fixture) == QList<int>({2, 6, 10}); });
            // Deciding the second removes the block, so nothing folds and no region has markers.
            resolve(SourceSide::Stage3, 1);
            CHECK(fixture.text() == QStringLiteral("header\nleft one\nshared\nright two\ntail\n"));
            CHECK(fixture.controller.selectedFile().value("markerBlocks").toList().isEmpty());
            CHECK(remain().size() == atLaunch.size() && !remain().contains(true));
            settle([&] { return foldedLines(fixture).isEmpty(); });
            // One undo brings back the block and its fold.
            CHECK(fixture.controller.undo("choices", QString::number(fixture.file().current().id)));
            CHECK(fixture.text() == QString::fromStdString(test_support::splitText()) && remain() == atLaunch && folded());
            settle([&] { return foldedLines(fixture) == QList<int>({2, 6, 10}); });
        });
        suite.run("TRUCE-0058 whitespace markers paint spaces and tabs", [] {
            const std::string result = "aa bb\tcc\n";
            Fixture fixture(test_support::choiceFile("left\n", "right\n", result));
            const auto before = captureWork(fixture);
            auto *markers = fixture.item("resultWhitespace");
            auto *toggle = fixture.item("showWhitespace");
            CHECK(!toggle->property("checked").toBool());
            settle([&] { return markers->property("markerCount").toInt() == 0 && paintedMarkers(markers) == 0; });
            CHECK(markerKind(markers, false) == 0 && markerKind(markers, true) == 0);
            CHECK(toggle->setProperty("checked", true));
            settle([&] { return markers->property("markerCount").toInt() == 2 && paintedMarkers(markers) == 2; });
            CHECK(markerKind(markers, false) == 1 && markerKind(markers, true) == 1);
            CHECK(fixture.item("stage2Whitespace")->property("markerCount").toInt() == 0);
            CHECK(fixture.item("stage3Whitespace")->property("markerCount").toInt() == 0);
            CHECK(fixture.item("resultText")->property("text").toString().toStdString() == result);
            checkUnchanged(fixture, before);
            CHECK(toggle->setProperty("checked", false));
            settle([&] { return markers->property("markerCount").toInt() == 0 && paintedMarkers(markers) == 0; });
            CHECK(fixture.item("resultText")->property("text").toString().toStdString() == result);
            checkUnchanged(fixture, before);
        });
        suite.run("TRUCE-0118 the undecided counter steps to the next undecided choice and counts down as choices are decided", [] {
            const std::string result = "same\nAAA\nmid\nBBB\n";
            const std::string left = "same\nleftA\nmid\nleftB\n";
            Fixture fixture(test_support::choiceFile(left, result, result));
            CHECK(fixture.file().facts().undecided == 4);
            auto *counter = fixture.item("undecidedCounter");
            settle([&] { return counter->property("text").toString() == QStringLiteral("4 undecided"); });
            CHECK(counter->isEnabled());

            fixture.select(0, 0);
            fixture.click("undecidedCounter");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 0, 4, 5, 9,
                QStringLiteral("Undecided 1 of 4 · Left · result bytes 5-9"),
                QStringLiteral("pick/1/5/11"));
            fixture.click("undecidedCounter");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 1, 4, 5, 9,
                QStringLiteral("Undecided 2 of 4 · Right · result bytes 5-9"),
                QStringLiteral("pick/2/5/9"));

            // Ignoring the left side of the first region decides one choice.
            const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
            CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                QString::fromStdString(target.source.contributionId), "ignore"));
            CHECK(fixture.controller.applyChoice(true));
            CHECK(fixture.file().facts().undecided == 3);
            settle([&] { return counter->property("text").toString() == QStringLiteral("3 undecided"); });
            CHECK(counter->isEnabled());
        });
        suite.run("TRUCE-0059 revision progress, within-file navigation, unfinished files and file-list location", [] {
            const std::string result = "same\nAAA\nmid\nBBB\n";
            const std::string left = "same\nleftA\nmid\nleftB\n";
            Fixture fixture(test_support::choiceFile(left, result, result));
            const auto revision = fixture.file().current().id;
            CHECK(revision == 1);
            CHECK(fixture.file().facts().undecided == 4);
            expectProgress(fixture, false, false, false, false,
                QStringLiteral("1"), {}, {}, {}, 4);

            fixture.select(0, 0);
            fixture.click("nextDifference");
            expectNavigation(fixture, QStringLiteral("difference"), QStringLiteral("choices"),
                QStringLiteral("result"), 0, 2, 5, 9,
                QStringLiteral("Difference 1 of 2 · result bytes 5-9"));
            fixture.click("nextDifference");
            expectNavigation(fixture, QStringLiteral("difference"), QStringLiteral("choices"),
                QStringLiteral("result"), 1, 2, 13, 17,
                QStringLiteral("Difference 2 of 2 · result bytes 13-17"));
            fixture.click("nextDifference");
            expectNavigation(fixture, QStringLiteral("difference"), QStringLiteral("choices"),
                QStringLiteral("result"), 0, 2, 5, 9,
                QStringLiteral("Difference 1 of 2 · result bytes 5-9"));
            fixture.click("previousDifference");
            expectNavigation(fixture, QStringLiteral("difference"), QStringLiteral("choices"),
                QStringLiteral("result"), 1, 2, 13, 17,
                QStringLiteral("Difference 2 of 2 · result bytes 13-17"));

            fixture.select(0, 0);
            fixture.click("nextUndecided");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 0, 4, 5, 9,
                QStringLiteral("Undecided 1 of 4 · Left · result bytes 5-9"),
                QStringLiteral("pick/1/5/11"));
            fixture.click("nextUndecided");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 1, 4, 5, 9,
                QStringLiteral("Undecided 2 of 4 · Right · result bytes 5-9"),
                QStringLiteral("pick/2/5/9"));
            fixture.click("nextUndecided");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 2, 4, 13, 17,
                QStringLiteral("Undecided 3 of 4 · Left · result bytes 13-17"),
                QStringLiteral("pick/1/15/21"));
            fixture.click("nextUndecided");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 3, 4, 13, 17,
                QStringLiteral("Undecided 4 of 4 · Right · result bytes 13-17"),
                QStringLiteral("pick/2/13/17"));
            fixture.click("nextUndecided");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 0, 4, 5, 9,
                QStringLiteral("Undecided 1 of 4 · Left · result bytes 5-9"),
                QStringLiteral("pick/1/5/11"));
            fixture.click("previousUndecided");
            expectNavigation(fixture, QStringLiteral("undecided"), QStringLiteral("choices"),
                QStringLiteral("result"), 3, 4, 13, 17,
                QStringLiteral("Undecided 4 of 4 · Right · result bytes 13-17"),
                QStringLiteral("pick/2/13/17"));
            CHECK(fixture.controller.selectedFileId() == QStringLiteral("choices"));
            CHECK(fixture.file().current().id == revision);
            CHECK(fixture.text().toStdString() == result);

            CHECK(fixture.item("saveDraft")->isEnabled());
            fixture.click("saveDraft");
            CHECK(fixture.controller.commandMessage()
                == QStringLiteral("Draft saved in Branch Truce; repository file unchanged."));
            CHECK(fixture.file().checkpoint() && fixture.file().checkpoint()->id == revision);
            CHECK(!fixture.file().written() && !fixture.file().staged() && !fixture.file().reviewed());
            expectProgress(fixture, true, false, false, false,
                QStringLiteral("1"), QStringLiteral("1"), {}, {}, 4);

            // Progress keeps the review, write and stage revisions separate.
            CHECK(fixture.controller.files()->beginMarkResolved("choices", revision, true) == Transition::Applied);
            CHECK(fixture.file().reviewed() && fixture.file().reviewed()->revision.id == revision);
            CHECK(!fixture.file().written() && !fixture.file().staged() && !fixture.file().facts().resolved);
            expectProgress(fixture, true, true, false, false,
                QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), {}, 4);
            CHECK(fixture.controller.files()->recordWrite("choices", revision, Outcome::Succeeded) == Transition::Applied);
            CHECK(fixture.controller.files()->recordStage("choices", revision, Outcome::Succeeded,
                "verified-object", "100644") == Transition::Applied);
            CHECK(fixture.file().facts().resolved && fixture.file().staged()->revision.id == revision);
            CHECK(fixture.text().toStdString() == result);
            expectProgress(fixture, true, true, true, false,
                QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), 4);

            CHECK(fixture.controller.files()->appendFile(
                FileSession("outside", "diagram.png", {}, Support::OutsideApp, "image content")));
            CHECK(fixture.controller.outsideAppFiles() == 1);
            CHECK(fixture.file().facts().resolved);
            fixture.clickMenuEntry("navigateMenu", "nextUnfinished");
            expectNavigation(fixture, QStringLiteral("unfinished"), QStringLiteral("outside"),
                QStringLiteral("outside-editor"), 0, 1, 0, 0,
                QStringLiteral("Unfinished file · outside the editor · diagram.png"));
            const auto unfinished = fixture.controller.navigationTarget();
            CHECK(unfinished.value(QStringLiteral("support")).toString() == QStringLiteral("outside-app"));
            CHECK(!unfinished.value(QStringLiteral("resolved")).toBool());
            CHECK(unfinished.value(QStringLiteral("path")).toString() == QStringLiteral("diagram.png"));
            CHECK(fixture.item("selectedProblem")->property("text").toString() == QStringLiteral("Outside app"));
            CHECK(fixture.item("selectedProblem")->property("explanation").toString().contains(
                QStringLiteral("outstanding")));
            CHECK(fixture.item("selectedProblem")->property("explanation").toString().contains(
                QStringLiteral("image content")));
            const auto *outside = fixture.controller.files()->file("outside");
            CHECK(outside && outside->support() == Support::OutsideApp);
            CHECK(outside->supportDetail() == "image content");
            CHECK(outside->current().draft.text.empty() && !outside->facts().resolved);
            fixture.clickMenuEntry("navigateMenu", "previousUnfinished");
            expectNavigation(fixture, QStringLiteral("unfinished"), QStringLiteral("outside"),
                QStringLiteral("outside-editor"), 0, 1, 0, 0,
                QStringLiteral("Unfinished file · outside the editor · diagram.png"));
            CHECK(fixture.file().support() == Support::Supported && fixture.file().facts().resolved);

            CHECK(fixture.controller.selectFile(QStringLiteral("choices")));
            settle([&] { return fixture.text().toStdString() == result; });
            auto newer = fixture.file().current().draft;
            newer.text += "newer\n";
            CHECK(fixture.controller.files()->replaceDraft("choices", revision, newer) == Transition::Applied);
            CHECK(fixture.file().current().id == revision + 1);
            CHECK(fixture.file().staged() && fixture.file().staged()->revision.id == revision);
            CHECK(fixture.file().facts().earlierStaged && !fixture.file().facts().resolved && fixture.file().facts().unsaved);
            settle([&] { return fixture.text().toStdString() == newer.text; });
            expectProgress(fixture, false, false, false, true,
                QStringLiteral("2"), {}, {}, QStringLiteral("1"), 4);

            const auto heldRevision = fixture.file().current().id;
            const auto heldText = fixture.text();
            fixture.select(1, 6);
            auto *editor = fixture.item("resultText");
            const int cursor = editor->property("cursorPosition").toInt();
            const int selectionStart = editor->property("selectionStart").toInt();
            const int selectionEnd = editor->property("selectionEnd").toInt();
            if (cursor != 6 || selectionStart != 1 || selectionEnd != 6)
                throw std::runtime_error("held selection " + std::to_string(selectionStart)
                    + "-" + std::to_string(selectionEnd) + " cursor " + std::to_string(cursor));
            CHECK(cursor == 6 && selectionStart == 1 && selectionEnd == 6);
            CHECK(fixture.controller.fileListVisible() && fixture.item("fileList")->isVisible());
            CHECK(fixture.item("toggleFileList")->property("checked").toBool());
            CHECK(test_support::accessibleName(fixture.item("toggleFileList")) == QStringLiteral("Show file list"));
            fixture.click("toggleFileList");
            CHECK(!fixture.controller.fileListVisible() && !fixture.item("fileList")->isVisible());
            CHECK(!fixture.item("toggleFileList")->property("checked").toBool());
            CHECK(test_support::accessibleName(fixture.item("toggleFileList")) == QStringLiteral("Show file list"));
            CHECK(fixture.controller.selectedFileId() == QStringLiteral("choices"));
            CHECK(fixture.file().current().id == heldRevision && fixture.file().facts().unsaved);
            CHECK(fixture.text() == heldText && fixture.text().toStdString() == newer.text);
            CHECK(editor->property("cursorPosition").toInt() == cursor);
            CHECK(editor->property("selectionStart").toInt() == selectionStart);
            CHECK(editor->property("selectionEnd").toInt() == selectionEnd);
            fixture.click("toggleFileList");
            CHECK(fixture.controller.fileListVisible() && fixture.item("fileList")->isVisible());
            CHECK(fixture.item("toggleFileList")->property("checked").toBool());
            CHECK(test_support::accessibleName(fixture.item("toggleFileList")) == QStringLiteral("Show file list"));
            CHECK(fixture.controller.selectedFileId() == QStringLiteral("choices"));
            CHECK(fixture.file().current().id == heldRevision && fixture.text() == heldText && fixture.file().facts().unsaved);
            CHECK(editor->property("cursorPosition").toInt() == cursor);
            CHECK(editor->property("selectionStart").toInt() == selectionStart);
            CHECK(editor->property("selectionEnd").toInt() == selectionEnd);
            const auto *stillOutside = fixture.controller.files()->file("outside");
            CHECK(stillOutside && stillOutside->support() == Support::OutsideApp);
            CHECK(stillOutside->supportDetail() == "image content" && stillOutside->current().draft.text.empty());
        });
        suite.run("TRUCE-0132 toggling the file list restores the result viewport scroll position", [] {
            std::string result;
            for (int line = 0; line < 200; ++line) result += std::string(240, static_cast<char>('a' + line % 26)) + "\n";
            Fixture fixture(test_support::choiceFile("left\n", result, result));
            auto *viewport = fixture.item("resultViewport");
            settle([&] { return viewport->property("contentHeight").toReal() > viewport->height() * 2
                && viewport->property("contentWidth").toReal() > viewport->width() * 2; });
            // Keep the cursor at the top so that scrolling away leaves it out of view.
            fixture.select(0, 0);
            CHECK(viewport->setProperty("contentY", 600.0));
            CHECK(viewport->setProperty("contentX", 40.0));
            settle([&] { return viewport->property("contentY").toReal() == 600.0
                && viewport->property("contentX").toReal() == 40.0; });
            // Move the viewport between the note and the restore, as the relayout does.
            const auto moved = QObject::connect(&fixture.controller, &ApplicationController::fileListChanged, viewport,
                [viewport] {
                    viewport->setProperty("contentY", 0.0);
                    viewport->setProperty("contentX", 0.0);
                }, Qt::DirectConnection);
            fixture.click("toggleFileList");
            QObject::disconnect(moved);
            CHECK(!fixture.controller.fileListVisible());
            settle([&] { return fixture.item("resultText")->width() > 0; });
            CHECK(viewport->property("contentY").toReal() == 600.0);
            CHECK(viewport->property("contentX").toReal() == 40.0);
            fixture.click("toggleFileList");
            CHECK(fixture.controller.fileListVisible());
            settle([&] { return fixture.item("resultText")->width() > 0; });
            CHECK(viewport->property("contentY").toReal() == 600.0);
            CHECK(viewport->property("contentX").toReal() == 40.0);
        });
        suite.run("TRUCE-0109 dragging the split handle resizes the file list within its limits and the toggle collapses it", [] {
            Fixture fixture;
            auto *sidebar = test_support::waitForLayout(fixture.window, QStringLiteral("fileList"), 60)->parentItem();
            auto *split = sidebar->parentItem();
            while (split && !QByteArray(split->metaObject()->className()).contains("SplitView")) split = split->parentItem();
            CHECK(split);
            const auto minimum = qmlValue(sidebar, QStringLiteral("SplitView.minimumWidth")).toReal();
            const auto maximum = [&] { return qmlValue(sidebar, QStringLiteral("SplitView.maximumWidth")).toReal(); };
            // The split puts its handles beside the items, as its own children.
            const auto handle = [&]() -> QQuickItem * {
                const auto edge = sidebar->mapToScene(QPointF(sidebar->width(), 0)).x();
                for (auto *child : split->childItems()) {
                    if (child->isVisible() && child->width() > 0 && child->width() < 40
                            && std::abs(child->mapToScene(QPointF()).x() - edge) < 2)
                        return child;
                }
                throw std::runtime_error("no split handle beside the file list");
            };
            // Drags the handle by a distance, kept inside the window, a few pixels at a time.
            const auto drag = [&](int by) {
                auto *grip = handle();
                const auto from = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
                const auto to = std::clamp(from.x() + by, 5, fixture.window->width() - 5);
                QTest::mousePress(fixture.window, Qt::LeftButton, Qt::NoModifier, from);
                for (int step = 1; step <= 10; ++step)
                    QTest::mouseMove(fixture.window, QPoint(from.x() + (to - from.x()) * step / 10, from.y()));
                QTest::mouseRelease(fixture.window, Qt::LeftButton, Qt::NoModifier, QPoint(to, from.y()));
                for (int turn = 0; turn < 10; ++turn) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            };
            const auto start = sidebar->width();
            CHECK(start >= minimum && start + 80 < maximum());
            drag(80);
            CHECK(std::abs(sidebar->width() - (start + 80)) < 2);
            // It stops at half the work area, and at its minimum.
            drag(fixture.window->width());
            CHECK(std::abs(sidebar->width() - maximum()) < 1);
            drag(-fixture.window->width());
            CHECK(std::abs(sidebar->width() - minimum) < 1);
            drag(120);
            const auto chosen = sidebar->width();
            CHECK(std::abs(chosen - (minimum + 120)) < 2);
            fixture.click("toggleFileList");
            CHECK(!fixture.controller.fileListVisible());
            settle([&] { return !sidebar->isVisible(); });
            fixture.click("toggleFileList");
            CHECK(fixture.controller.fileListVisible());
            settle([&] { return sidebar->isVisible() && sidebar->width() > 0; });
            CHECK(std::abs(sidebar->width() - chosen) < 2);
        });
        suite.run("TRUCE-0115 scrolling the left source moves the result and the right source to the mapped lines", [] {
            Fixture fixture(insertionFile());
            auto *left = fixture.item("stage2Viewport");
            auto *result = fixture.item("resultViewport");
            auto *right = fixture.item("stage3Viewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
            settlePanes(fixture);
            const auto line = paneLineHeight(editor);
            // With every visible left line past the insertion, the result and the
            // right source show the same lines 30 lines further down.
            wheelTo(fixture, "stage2Viewport", 80 * line);
            CHECK(std::abs(contentY(result) - (contentY(left) + 30 * line)) < 1);
            CHECK(std::abs(contentY(right) - contentY(result)) < 1);
            // Nothing scrolls on once the movement has ended.
            const std::array<qreal, 3> settled{contentY(left), contentY(result), contentY(right)};
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            CHECK((std::array<qreal, 3>{contentY(left), contentY(result), contentY(right)}) == settled);

            // Five new lines at the top of the result move the mapping with them.
            fixture.select(0, 0);
            for (int typed = 0; typed < 5; ++typed)
                fixture.key(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            settle([&] { return fixture.file().current().draft.text.starts_with("\n\n\n\n\nshared 1\n"); });
            wheelTo(fixture, "stage2Viewport", 120 * line);
            CHECK(std::abs(contentY(result) - (contentY(left) + 35 * line)) < 1);
            CHECK(std::abs(contentY(right) - (contentY(left) + 30 * line)) < 1);
        });
        suite.run("TRUCE-0115 a synchronised scroll settles in the same event-loop turn without feedback", [] {
            Fixture fixture(insertionFile());
            auto *left = fixture.item("stage2Viewport");
            auto *result = fixture.item("resultViewport");
            auto *right = fixture.item("stage3Viewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
            settlePanes(fixture);
            const auto line = paneLineHeight(editor);
            // The left source has keyboard focus, so its own scrolling leads.
            fixture.item("stage2Text")->forceActiveFocus();
            for (const qreal top : {100 * line, 140 * line, 90 * line}) {
                CHECK(left->setProperty("contentY", top));
                // Read before any event is processed: the other panes moved in the same turn.
                CHECK(std::abs(contentY(result) - (top + 30 * line)) < 1);
                CHECK(std::abs(contentY(right) - (top + 30 * line)) < 1);
                for (int turn = 0; turn < 10; ++turn) QCoreApplication::processEvents();
                CHECK(contentY(left) == top);
                CHECK(std::abs(contentY(result) - (top + 30 * line)) < 1);
                CHECK(std::abs(contentY(right) - (top + 30 * line)) < 1);
            }
            // A pane the user is not scrolling does not lead the others.
            const auto held = contentY(left);
            CHECK(right->setProperty("contentY", contentY(right) + 10 * line));
            for (int turn = 0; turn < 10; ++turn) QCoreApplication::processEvents();
            CHECK(contentY(left) == held);
            CHECK(std::abs(contentY(result) - (held + 30 * line)) < 1);
        });
        suite.run("TRUCE-0115 an ambiguous passage scrolls the other panes proportionally without jumping", [] {
            // 40 equal lines on the left against 20 in the result cannot be aligned line by line.
            const auto before = numberedLines("before", 1, 80);
            const auto after = numberedLines("after", 1, 200);
            std::string left = before;
            std::string result = before;
            for (int line = 0; line < 40; ++line) left += "same\n";
            for (int line = 0; line < 20; ++line) result += "same\n";
            left += after;
            result += after;
            Fixture fixture(test_support::choiceFile(left, result, result));
            bool ambiguous = false;
            for (const auto &value : fixture.controller.selectedFile().value("passages").toList()) {
                const auto passage = value.toMap();
                ambiguous = ambiguous || (passage.value("side") == "stage2" && passage.value("kind") == "ambiguous"
                    && passage.value("sourceLineBegin").toInt() <= 81 && passage.value("sourceLineEnd").toInt() >= 121);
            }
            CHECK(ambiguous);
            auto *leftView = fixture.item("stage2Viewport");
            auto *resultView = fixture.item("resultViewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 300 && leftView->height() > 0; });
            settlePanes(fixture);
            const auto line = paneLineHeight(editor);
            fixture.item("stage2Text")->forceActiveFocus();
            CHECK(leftView->setProperty("contentY", 30 * line));
            CHECK(std::abs(contentY(resultView) - 30 * line) < 1);
            qreal previous = contentY(resultView);
            for (qreal top = 30 * line + 4; top <= 170 * line; top += 4) {
                CHECK(leftView->setProperty("contentY", top));
                const auto now = contentY(resultView);
                // The result never runs backwards or ahead of the left.
                if (now < previous - 0.01 || now - previous > 4.01)
                    throw std::runtime_error("result jumped from " + std::to_string(previous) + " to "
                                             + std::to_string(now) + " at left " + std::to_string(top));
                previous = now;
            }
            // Past the passage the result is 20 lines behind the left.
            CHECK(std::abs(contentY(resultView) - (contentY(leftView) - 20 * line)) < 1);
        });
        suite.run("TRUCE-0115 the shown earlier version scrolls with the other panes", [] {
            auto sources = *insertionFile().snapshots();
            const auto earlier = sources.stage2->worktree.utf8;
            sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
                test_support::choiceText(earlier), "earlier-commit", "Common earlier version"};
            const auto launch = sources.launchResult.text.utf8;
            Fixture fixture(FileSession("choices", sources.path, {launch, {}, 0}, Support::Supported, {}, sources, true));
            fixture.click("showEarlier");
            auto *left = fixture.item("stage2Viewport");
            auto *earlierView = fixture.item("earlierViewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return earlierView->height() > 0 && editor->property("lineCount").toInt() > 400; });
            const auto line = paneLineHeight(editor);
            wheelTo(fixture, "stage2Viewport", 80 * line);
            // The earlier version equals the left source, so it shows the same lines.
            CHECK(std::abs(contentY(earlierView) - contentY(left)) < 1);
        });
        suite.run("TRUCE-0164 the earlier version catches up with the other panes when it is first shown", [] {
            auto sources = *insertionFile().snapshots();
            const auto earlier = sources.stage2->worktree.utf8;
            sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
                test_support::choiceText(earlier), "earlier-commit", "Common earlier version"};
            const auto launch = sources.launchResult.text.utf8;
            Fixture fixture(FileSession("choices", sources.path, {launch, {}, 0}, Support::Supported, {}, sources, true));
            auto *left = fixture.item("stage2Viewport");
            auto *earlierView = fixture.item("earlierViewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
            settlePanes(fixture);
            CHECK(!earlierView->isVisible());
            const auto line = paneLineHeight(editor);
            // The panes scroll while the earlier version is hidden, so it is still at the top.
            wheelTo(fixture, "stage2Viewport", 80 * line);
            CHECK(contentY(earlierView) == 0);
            fixture.click("showEarlier");
            settleFor("the earlier version to catch up", [&] {
                return earlierView->isVisible() && earlierView->height() > 0
                    && std::abs(contentY(earlierView) - contentY(left)) < 1;
            });
            // It equals the left source, so it shows the left source's lines.
            CHECK(contentY(earlierView) > 70 * line);
            CHECK(std::abs(contentY(earlierView) - contentY(left)) < 1);
        });
        suite.run("TRUCE-0115 Find in a source scrolls the result and the other source to the passage", [] {
            Fixture fixture(insertionFile());
            auto *left = fixture.item("stage2Viewport");
            auto *result = fixture.item("resultViewport");
            auto *right = fixture.item("stage3Viewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 400 && left->height() > 0; });
            settlePanes(fixture);
            const auto line = paneLineHeight(editor);
            CHECK(contentY(left) < 200 * line);
            CHECK(fixture.item("searchScope")->property("currentText").toString() == "Left source");
            CHECK(fixture.item("searchQuery")->setProperty("text", "shared 300"));
            fixture.click("findPassage");
            auto *found = fixture.item("stage2Text");
            CHECK(found->property("selectedText").toString() == "shared 300");
            settleReplacement();
            // The left source shows the passage, and the others the lines mapped to it.
            const auto cursor = found->property("cursorRectangle").toRectF();
            const auto shown = found->y() + cursor.top() - contentY(left);
            CHECK(shown >= 0 && shown + cursor.height() <= left->height());
            CHECK(contentY(left) > 200 * line);
            CHECK(std::abs(contentY(result) - (contentY(left) + 30 * line)) < 1);
            CHECK(std::abs(contentY(right) - contentY(result)) < 1);
        });
        suite.run("TRUCE-0115 within-file navigation scrolls both sources to the result's difference", [] {
            const auto left = numberedLines("shared", 1, 400);
            const auto right = numberedLines("shared", 1, 50) + numberedLines("right only", 1, 30)
                + numberedLines("shared", 51, 249) + "right late\n" + numberedLines("shared", 301, 100);
            Fixture fixture(test_support::choiceFile(left, right, right));
            auto *leftView = fixture.item("stage2Viewport");
            auto *result = fixture.item("resultViewport");
            auto *rightView = fixture.item("stage3Viewport");
            auto *editor = fixture.item("resultText");
            settle([&] { return editor->property("lineCount").toInt() > 400 && leftView->height() > 0; });
            settlePanes(fixture);
            const auto line = paneLineHeight(editor);
            const auto late = static_cast<int>(fixture.text().indexOf("right late"));
            CHECK(late > 0);
            fixture.select(0, 0);
            for (int click = 0; click < 5 && editor->property("selectionStart").toInt() != late; ++click)
                fixture.click("nextDifference");
            CHECK(editor->property("selectionStart").toInt() == late);
            settleReplacement();
            CHECK(cursorInView(fixture));
            CHECK(contentY(result) > 250 * line);
            // Past the insertion the left source is 30 lines behind; the right source equals the result.
            CHECK(std::abs(contentY(leftView) - (contentY(result) - 30 * line)) < 1);
            CHECK(std::abs(contentY(rightView) - contentY(result)) < 1);
        });
        const auto undoRedoKeepViewport = [] {
            Fixture fixture(insertionFile());
            auto *editor = fixture.item("resultText");
            auto *left = fixture.item("stage2Viewport");
            auto *result = fixture.item("resultViewport");
            auto *right = fixture.item("stage3Viewport");
            const auto line = paneLineHeight(editor);
            placeResultCursor(fixture, QStringLiteral("shared 20\n"), 10 * line);
            const auto offset = editor->property("cursorPosition").toInt();
            fixture.key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            settle([&] { return fixture.file().current().draft.text.find("xshared 20\n") != std::string::npos; });
            settleReplacement();
            CHECK(editor->property("cursorPosition").toInt() == offset + 1);
            const std::array<QQuickItem *, 3> panes{left, result, right};
            const std::array<qreal, 3> typed{contentY(left), contentY(result), contentY(right)};
            CHECK(std::abs(typed[1] - 10 * line) < 1);

            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft.text.find("xshared") == std::string::npos);
            settleReplacement();
            CHECK(editor->property("cursorPosition").toInt() == offset);
            CHECK(editor->property("selectionStart").toInt() == editor->property("selectionEnd").toInt());
            // No pane follows the replaced text to its end.
            for (std::size_t pane = 0; pane < panes.size(); ++pane) CHECK(std::abs(contentY(panes[pane]) - typed[pane]) < 1);

            fixture.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(fixture.file().current().draft.text.find("xshared 20\n") != std::string::npos);
            settleReplacement();
            // The cursor follows the restored insertion, as it did when the character was typed.
            CHECK(editor->property("cursorPosition").toInt() == offset + 1);
            for (std::size_t pane = 0; pane < panes.size(); ++pane) CHECK(std::abs(contentY(panes[pane]) - typed[pane]) < 1);
        };
        // The view restores the result viewport once, with no reapply after the next frame, so
        // let the window draw frames after the undo before reading the panes.
        const auto undoKeepsViewportAfterFrames = [] {
            Fixture fixture(insertionFile());
            auto *editor = fixture.item("resultText");
            auto *result = fixture.item("resultViewport");
            const auto line = paneLineHeight(editor);
            placeResultCursor(fixture, QStringLiteral("shared 200\n"), 220 * line);
            const auto offset = editor->property("cursorPosition").toInt();
            CHECK(cursorInView(fixture));
            fixture.key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            settle([&] { return fixture.file().current().draft.text.find("xshared 200\n") != std::string::npos; });
            settleReplacement();
            const auto typed = contentY(result);
            CHECK(std::abs(typed - 220 * line) < 1);
            int frames = 0;
            const auto counter = QObject::connect(fixture.window, &QQuickWindow::frameSwapped, fixture.window,
                [&frames] { ++frames; });
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            CHECK(fixture.file().current().draft.text.find("xshared") == std::string::npos);
            settleFor("three frames after the undo", [&] {
                if (frames < 3) fixture.window->update();
                return frames >= 3;
            });
            QObject::disconnect(counter);
            settleReplacement();
            CHECK(editor->property("cursorPosition").toInt() == offset);
            CHECK(cursorInView(fixture));
            if (std::abs(contentY(result) - typed) >= 1)
                throw std::runtime_error("result viewport moved from " + std::to_string(typed) + " to "
                    + std::to_string(contentY(result)) + " after " + std::to_string(frames) + " frames");
        };
        const std::string undoRedoName = "TRUCE-0137 undo and redo keep the result cursor and scroll position, and the sources stay";
        const std::string framesName = "TRUCE-0137 undo keeps the result viewport after the window draws";
        suite.run(undoRedoName, undoRedoKeepViewport);
        suite.run(framesName, undoKeepsViewportAfterFrames);
        display.run(undoRedoName, undoRedoKeepViewport);
        display.run(framesName, undoKeepsViewportAfterFrames);
        suite.run("TRUCE-0137 undo keeps a result selection on the same text", [] {
            Fixture fixture(insertionFile());
            auto *editor = fixture.item("resultText");
            auto *result = fixture.item("resultViewport");
            const auto line = paneLineHeight(editor);
            placeResultCursor(fixture, QStringLiteral("shared 20\n"), 10 * line);
            fixture.key(Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            settle([&] { return fixture.file().current().draft.text.find("xshared 20\n") != std::string::npos; });
            // Select "shared 22" backwards, so the cursor is at its start.
            const auto selected = static_cast<int>(fixture.text().indexOf(QStringLiteral("shared 22")));
            fixture.select(selected + 9, selected);
            fixture.key(Qt::Key_Z, Qt::ControlModifier);
            settleReplacement();
            CHECK(fixture.text().indexOf(QStringLiteral("shared 22")) == selected - 1);
            CHECK(editor->property("selectionStart").toInt() == selected - 1);
            CHECK(editor->property("selectionEnd").toInt() == selected + 8);
            CHECK(editor->property("cursorPosition").toInt() == selected - 1);
            CHECK(std::abs(contentY(result) - 10 * line) < 1);
        });
        suite.run("TRUCE-0137 applying a choice keeps the result cursor and scroll position", [] {
            Fixture fixture(insertionFile());
            auto *editor = fixture.item("resultText");
            auto *result = fixture.item("resultViewport");
            const auto line = paneLineHeight(editor);
            // Above the 30 inserted lines that the left source lacks.
            placeResultCursor(fixture, QStringLiteral("shared 20\n"), 10 * line);
            const auto offset = editor->property("cursorPosition").toInt();
            const auto before = fixture.text();
            const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
            CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                QString::fromStdString(target.source.contributionId), QStringLiteral("resolve")));
            CHECK(fixture.controller.applyChoice(true));
            CHECK(fixture.text() != before && !fixture.text().contains(QStringLiteral("right only")));
            settleReplacement();
            CHECK(editor->property("cursorPosition").toInt() == offset);
            CHECK(std::abs(contentY(result) - 10 * line) < 1);
        });
        suite.run("TRUCE-0137 a choice above the result cursor keeps it on its text and in view", [] {
            Fixture fixture(insertionFile());
            auto *editor = fixture.item("resultText");
            const auto line = paneLineHeight(editor);
            // Below the 30 inserted lines, with the cursor near the top of the view.
            placeResultCursor(fixture, QStringLiteral("shared 120\n"), 148 * line);
            CHECK(cursorInView(fixture));
            const auto &target = test_support::choiceTarget(fixture.file(), SourceSide::Stage2, ChoiceScope::Section);
            CHECK(fixture.controller.previewChoice("choices", QString::number(fixture.file().current().id),
                QString::fromStdString(target.source.contributionId), QStringLiteral("resolve")));
            CHECK(fixture.controller.applyChoice(true));
            CHECK(!fixture.text().contains(QStringLiteral("right only")));
            settleReplacement();
            // The removed lines took the cursor's line out of the kept view, so the view follows it.
            CHECK(editor->property("cursorPosition").toInt() == fixture.text().indexOf(QStringLiteral("shared 120\n")));
            CHECK(cursorInView(fixture));
        });
        suite.run("TRUCE-0142 two separate changes keep the result cursor on the unchanged text between them", [] {
            // The sources differ on lines 20 and 350, and the result holds the right source.
            const auto first = numberedLines("shared", 1, 19);
            const auto middle = numberedLines("shared", 21, 329);
            const auto last = numberedLines("shared", 351, 50);
            const auto left = first + "left first\n" + middle + "left last\n" + last;
            const auto right = first + "right first\n" + middle + "right last\n" + last;
            Fixture fixture(test_support::choiceFile(left, right, right));
            auto *editor = fixture.item("resultText");
            auto *result = fixture.item("resultViewport");
            const auto line = paneLineHeight(editor);
            const auto marker = QStringLiteral("shared 150\n");
            placeResultCursor(fixture, marker, 140 * line);
            const auto revision = [&] { return QString::number(fixture.file().current().id); };
            const auto kept = [&] {
                settleReplacement();
                CHECK(editor->property("cursorPosition").toInt() == fixture.text().indexOf(marker));
                CHECK(editor->property("selectionStart").toInt() == editor->property("selectionEnd").toInt());
                CHECK(std::abs(contentY(result) - 140 * line) < 1);
            };
            CHECK(fixture.controller.previewWholeFile("choices", revision(), QStringLiteral("left")));
            CHECK(fixture.controller.applyWholeFile());
            CHECK(fixture.text().contains(QStringLiteral("left first\n")) && fixture.text().contains(QStringLiteral("left last\n")));
            kept();
            CHECK(fixture.controller.undo("choices", revision()));
            CHECK(fixture.text().contains(QStringLiteral("right first\n")) && fixture.text().contains(QStringLiteral("right last\n")));
            kept();
            CHECK(fixture.controller.redo("choices", revision()));
            CHECK(fixture.text().contains(QStringLiteral("left first\n")) && fixture.text().contains(QStringLiteral("left last\n")));
            kept();
        });
        suite.run("TRUCE-0060 a failed Discard shows the restored result and retry finishes it", [] {
            Fixture fixture;
            auto saved = fixture.file().current().draft;
            saved.text += "saved app draft\n";
            saved.decisions["kept"] = {DecisionValue::Accepted, 4};
            CHECK(fixture.controller.files()->replaceDraft("choices", fixture.file().current().id, saved) == Transition::Applied);
            settle([&] { return fixture.controller.selectedFile().value("unsaved").toBool(); });
            fixture.click("saveDraft");
            auto discarded = saved;
            discarded.text += "discarded edit\n";
            discarded.decisions["kept"] = {DecisionValue::Ignored, {}};
            CHECK(fixture.controller.files()->replaceDraft("choices", fixture.file().current().id, discarded) == Transition::Applied);
            settle([&] { return fixture.controller.selectedFile().value("unsaved").toBool(); });
            {
                recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
                fixture.click("discardUnsaved");
                CHECK(recovery_faults::failures == 1);
            }
            settle([&] { return fixture.controller.files()->hasPendingRecovery(); });
            const auto message = fixture.controller.commandMessage();
            CHECK(message == "Discard was not completed. The restored result is shown until retry finishes it.");
            CHECK(!message.contains(QStringLiteral("edited result is unchanged")));
            CHECK(fixture.text().toStdString() == saved.text);
            CHECK(fixture.controller.selectedFile().value("pendingText").toString().toStdString() == saved.text);
            CHECK(fixture.item("pendingInputText")->property("text").toString().toStdString() == saved.text);
            CHECK(fixture.file().current().draft == discarded);
            CHECK(fixture.controller.selectedFile().value("resultText").toString().toStdString() == discarded.text);
            CHECK(fixture.item("retryRecovery")->isVisible() && fixture.item("retryRecovery")->isEnabled());
            CHECK(!fixture.item("discardUnsaved")->isEnabled() && !fixture.item("markResolved")->isEnabled());
            storage::RecoveryStore store(fixture.controller.files()->recoveryDirectory());
            const auto pending = store.read();
            CHECK(pending.succeeded && pending.transactions.back().value("action") == "discard-to-checkpoint");
            const auto records = pending.transactions.size();
            const auto request = pending.transactions.back().value("transactionId");
            fixture.click("retryRecovery");
            settle([&] { return !fixture.controller.files()->hasPendingRecovery(); });
            CHECK(fixture.file().current().draft == saved && fixture.file().current().draft != discarded);
            CHECK(fixture.text().toStdString() == saved.text);
            CHECK(fixture.controller.commandMessage()
                  == "Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.");
            CHECK(!fixture.file().facts().unsaved && !fixture.file().written() && !fixture.file().staged());
            const auto committed = store.read();
            CHECK(committed.succeeded && committed.transactions.size() == records);
            CHECK(committed.transactions.back().value("transactionId") == request);
            CHECK(committed.transactions.back().value("action") == "discard-to-checkpoint");
            const auto replay = replayRecovery(storage::RecoveryStore(fixture.controller.files()->recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.front().current().draft == saved);
            CHECK(!replay.files.front().written() && !replay.files.front().staged());
        });
        suite.run("TRUCE-0026 Mark resolved confirms, rejects a stale revision and keeps a failed write visible", [] {
            Fixture fixture;
            const auto text = fixture.text();
            CHECK(fixture.file().facts().undecided > 0 && fixture.file().facts().unsaved && !fixture.file().reviewed());
            CHECK(fixture.item("markResolved")->isVisible() && fixture.item("markResolved")->isEnabled());
            CHECK(!fixture.item("confirmResolution")->isVisible());
            fixture.click("markResolved");
            CHECK(fixture.controller.resolutionConfirmation());
            CHECK(fixture.controller.commandMessage().contains(QStringLiteral("intentional")));
            CHECK(fixture.text() == text && !fixture.file().reviewed() && fixture.file().facts().unsaved);
            settle([&] { return fixture.item("confirmResolution")->isVisible() && fixture.item("cancelResolution")->isVisible(); });
            fixture.clickLaidOut("cancelResolution");
            CHECK(!fixture.controller.resolutionConfirmation());
            CHECK(fixture.controller.commandMessage().contains(QStringLiteral("unchanged")));
            CHECK(fixture.text() == text && fixture.file().current().draft.text == text.toStdString());
            CHECK(!fixture.controller.markResolved(QStringLiteral("choices"), QStringLiteral("999"), true));
            CHECK(fixture.controller.commandMessage().contains(QStringLiteral("changed")));
            CHECK(fixture.text() == text && !fixture.file().reviewed() && !fixture.file().facts().resolved);
            fixture.click("markResolved");
            fixture.clickLaidOut("confirmResolution");
            settle([&] {
                return fixture.controller.selectedFile().value(QStringLiteral("status")).toString()
                    == QStringLiteral("Repository write failed");
            });
            CHECK(fixture.text() == text);
            CHECK(fixture.file().reviewed() && fixture.file().checkpoint() && !fixture.file().written());
            CHECK(!fixture.file().facts().resolved && !fixture.file().staged());
            CHECK(test_support::shownText(fixture.item("selectedStatus")).contains(QStringLiteral("Repository write failed")));
            CHECK(!fixture.item("parentStagingState")->isVisible());
        });
        suite.run("TRUCE-0028 closing with work outstanding opens the handoff and each route returns to its file", [] {
            Fixture fixture;
            auto *files = fixture.controller.files();
            CHECK(files->appendFile(namedChoiceFile("saved")));
            CHECK(files->recordCheckpoint("saved", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(files->appendFile(namedChoiceFile("published")));
            CHECK(files->beginMarkResolved("published", 1, true) == Transition::Applied);
            CHECK(files->recordCheckpoint("published", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(files->recordWrite("published", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(files->recordStage("published", 1, Outcome::Succeeded, "verified-object", "100644")
                == Transition::Applied);
            CHECK(files->file("published")->facts().resolved);
            CHECK(files->appendFile(FileSession("outside", "diagram.png", {}, Support::OutsideApp, "image content")));

            auto *dialog = fixture.object("handoffDialog");
            const auto opened = [&] { return dialog->property("opened").toBool(); };
            CHECK(!dialog->property("visible").toBool());
            // The window close handler keeps the window and opens the handoff.
            fixture.window->close();
            settleFor("handoff opened by close", opened);
            CHECK(fixture.window->isVisible() && fixture.controller.sessionOutcome().isEmpty());
            CHECK(fixture.item("handoffOutcome")->property("text").toString()
                == QStringLiteral("3 of 4 files in this session remain outstanding. This session is not finished."));
            CHECK(!fixture.item("handoffFinish")->isVisible());

            QStringList outstanding;
            QMap<QString, QString> categories;
            for (const auto &group : fixture.controller.handoff().value(QStringLiteral("groups")).toList())
                for (const auto &entry : group.toMap().value(QStringLiteral("entries")).toList()) {
                    const auto fields = entry.toMap();
                    categories.insert(fields.value(QStringLiteral("fileId")).toString(),
                                      fields.value(QStringLiteral("category")).toString());
                    if (fields.value(QStringLiteral("outstanding")).toBool())
                        outstanding << fields.value(QStringLiteral("fileId")).toString();
                }
            CHECK(outstanding.size() == 3 && outstanding.contains(QStringLiteral("choices")));
            CHECK(categories.value(QStringLiteral("saved")) == QStringLiteral("saved-draft"));
            CHECK(categories.value(QStringLiteral("outside")) == QStringLiteral("outside-app"));
            CHECK(categories.value(QStringLiteral("published")) == QStringLiteral("published"));
            for (const auto &id : outstanding) {
                CHECK(fixture.controller.selectFile(QStringLiteral("published")));
                if (!opened()) fixture.click("openHandoff");
                settleFor("handoff reopened for " + id.toStdString(), opened);
                const auto route = QStringLiteral("handoffRoute-") + id;
                clickItem(fixture, waitForLayout(fixture, route));
                // Until its fade-out ends the closing dialog's overlay takes the next click.
                settleFor("handoff closed by route " + id.toStdString(), [&] { return !dialog->property("visible").toBool(); });
                if (fixture.controller.selectedFileId() != id)
                    throw std::runtime_error(("route " + id + " selected " + fixture.controller.selectedFileId()).toStdString());
                CHECK(fixture.controller.selectedFileId() == id);
            }
            CHECK(fixture.item("selectedProblem")->property("explanation").toString().contains(QStringLiteral("image content")));

            int ended = 0;
            bool clean = true;
            QObject::connect(&fixture.controller, &ApplicationController::sessionEnded, &fixture.controller,
                             [&](const QString &, bool value) { ++ended; clean = value; });
            fixture.click("openHandoff");
            settleFor("handoff opened for leave", opened);
            auto *leave = waitForLayout(fixture, QStringLiteral("handoffLeave"));
            CHECK(leave == fixture.item("handoffLeave") && leave->isEnabled());
            clickItem(fixture, leave);
            CHECK(ended == 1 && !clean);
            CHECK(fixture.controller.sessionOutcome().startsWith(QStringLiteral("Left with 3 of 4 files outstanding.")));
            CHECK(!fixture.controller.leaveSession() && ended == 1);
            CHECK(QMetaObject::invokeMethod(dialog, "close"));
            settleFor("handoff closed after leave", [&] { return !opened(); });
            // After the deliberate leave the close handler accepts the close.
            const bool quitOnClose = QGuiApplication::quitOnLastWindowClosed();
            QGuiApplication::setQuitOnLastWindowClosed(false);
            fixture.window->close();
            settleFor("window closed after leave", [&] { return !fixture.window->isVisible(); });
            QGuiApplication::setQuitOnLastWindowClosed(quitOnClose);
            CHECK(!opened() && ended == 1);
        });
        suite.run("TRUCE-0029 a rejected insertion at the file limit leaves the editor usable", [] {
            const auto atLimit = [] {
                std::string text;
                text.reserve(4000 * 2);
                for (int i = 0; i < 4000; ++i) text += "x\n";
                return text;
            }();
            Fixture fixture(test_support::choiceFile("x\n", "x\n", atLimit));
            auto *editor = fixture.item("resultText");
            const auto before = fixture.file().current();
            const auto rejection = QStringLiteral(
                "File of 4001 lines exceeds the supported 4000 line limit. "
                "The existing edit was kept and this change was not acknowledged.");
            CHECK(!editor->property("readOnly").toBool() && editor->isEnabled());
            CHECK(fixture.text().toStdString() == atLimit);
            CHECK(fixture.controller.commandMessage().isEmpty());
            fixture.select(0, 0);
            CHECK(editor->property("selectionStart").toInt() == 0
                && editor->property("selectionEnd").toInt() == 0);
            QGuiApplication::clipboard()->setText(QStringLiteral("\n"));
            CHECK(!fixture.controller.resultClipboard("paste"));
            CHECK(fixture.file().current().id == before.id && fixture.text().toStdString() == atLimit);
            CHECK(!editor->property("readOnly").toBool());
            CHECK(fixture.controller.commandMessage() == rejection);
            settle([&] {
                return fixture.item("commandMessage")->isVisible()
                    && fixture.item("commandMessage")->property("text").toString() == rejection;
            });
            fixture.checkRecovered(before.draft);
            bool timerFired = false;
            QTimer::singleShot(0, editor, [&] { timerFired = true; });
            fixture.key(Qt::Key_Return);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            CHECK(timerFired);
            CHECK(fixture.file().current().id == before.id && fixture.text().toStdString() == atLimit);
            CHECK(!editor->property("readOnly").toBool() && editor->isEnabled());
            CHECK(fixture.controller.commandMessage() == rejection);
            fixture.select(0, 1);
            fixture.key(Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
            const auto edited = fixture.file().current();
            CHECK(edited.id == before.id + 1);
            CHECK(edited.draft.text.size() == atLimit.size() && edited.draft.text.starts_with('y'));
            CHECK(fixture.text().toStdString() == edited.draft.text);
            CHECK(!editor->property("readOnly").toBool() && editor->isEnabled());
            CHECK(fixture.controller.commandMessage().isEmpty());
            settle([&] { return !fixture.item("commandMessage")->isVisible(); });
            fixture.checkRecovered(edited.draft);
            QGuiApplication::clipboard()->clear();
        });
        suite.run("TRUCE-0136 notices and tooltips show a path as typed and load nothing it names", [] {
            Fixture fixture;
            QTcpServer server;
            CHECK(server.listen(QHostAddress::LocalHost));
            // A file name cannot hold a slash, so the image address spells its slashes as entities.
            const auto path = QStringLiteral("<b>x</b><img src=\"http:&#47;&#47;127.0.0.1:%1&#47;probe\">.txt")
                .arg(server.serverPort()).toStdString();
            auto sources = *test_support::choiceFile().snapshots();
            sources.path = path;
            const auto launch = sources.launchResult.text.utf8;
            CHECK(fixture.controller.files()->appendFile(
                FileSession("crafted", path, {launch, {}, 0}, Support::Supported, {}, std::move(sources), true)));
            const auto shown = fixture.controller.files()->displayPath("crafted");
            CHECK(shown.toStdString() == path);
            const auto message = QStringLiteral("Waiting for Git to stage %1.").arg(shown);
            // Every label inside the item must show the message as plain characters. A label
            // that took it as rich text reports the markup it built instead.
            QStringList problems;
            const auto expectPlain = [&](QQuickItem *root, const QString &what) {
                int showing = 0;
                const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
                    for (auto *child : item->childItems()) visit(child);
                    if (!item->inherits("QQuickText") && !item->inherits("QQuickTextEdit")) return;
                    // Rich text reads back as the HTML it built, which still holds these words.
                    const auto text = item->property("text").toString();
                    if (!text.contains(QStringLiteral("Waiting for Git to stage"))) return;
                    ++showing;
                    const auto *document = item->property("textDocument").value<QQuickTextDocument *>();
                    const auto rendered = document ? document->textDocument()->toPlainText() : text;
                    if (item->property("textFormat").toInt() != Qt::PlainText || text != message || rendered != message)
                        problems.append(what + QStringLiteral(" shows ") + rendered);
                };
                visit(root);
                if (showing == 0) problems.append(what + QStringLiteral(" has no label"));
            };
            for (const auto *name : {"handoffOutcome", "inventoryWarning", "recoveryProblem", "commandMessage",
                     "parentStagingState", "mergetoolHolding", "mergetoolWait", "choiceNotice"}) {
                auto *notice = fixture.item(name);
                CHECK(notice->setProperty("visible", true) && notice->setProperty("text", message));
            }
            for (const auto *name : {"sessionPlaceholder", "selectedProblem"}) {
                auto *placeholder = fixture.item(name);
                CHECK(placeholder->setProperty("visible", true) && placeholder->setProperty("text", message)
                    && placeholder->setProperty("explanation", message));
            }
            // Attached tooltips share one popup per engine.
            auto *heading = fixture.item("repositoryHeading");
            QQmlExpression shared(qmlContext(heading), heading, QStringLiteral("ToolTip.toolTip"));
            auto *tooltip = shared.evaluate().value<QObject *>();
            CHECK(tooltip && !shared.hasError());
            CHECK(tooltip->setProperty("parent", QVariant::fromValue(heading)) && tooltip->setProperty("text", message)
                && tooltip->setProperty("visible", true));
            const auto requested = QTest::qWaitFor([&] { return server.hasPendingConnections(); }, 1000);
            server.close();
            for (const auto *name : {"handoffOutcome", "inventoryWarning", "recoveryProblem", "commandMessage",
                     "parentStagingState", "mergetoolHolding", "mergetoolWait", "choiceNotice", "sessionPlaceholder",
                     "selectedProblem"})
                expectPlain(fixture.item(name), QString::fromLatin1(name));
            auto *tooltipContent = tooltip->property("contentItem").value<QQuickItem *>();
            CHECK(tooltipContent);
            expectPlain(tooltipContent, QStringLiteral("tooltip"));
            if (requested) problems.append(QStringLiteral("a notice requested the image it names"));
            CHECK(tooltip->setProperty("visible", false));
            if (!problems.isEmpty()) throw std::runtime_error(problems.join(QStringLiteral("; ")).toStdString());
        });
        boundary.run("TRUCE-0063 Backspace and Delete shorten an exact 262144-byte line", [] {
            deletionAtLimit(std::string(262144, 'a'), QStringLiteral(
                "Line of 262145 bytes exceeds the supported 262144 byte line limit. "
                "The existing edit was kept and this change was not acknowledged."));
        });
        boundary.run("TRUCE-0063 Backspace and Delete shorten an exact 8388608-byte buffer", [] {
            // Lines stay below the line limit so only the buffer limit applies.
            std::string atLimit;
            atLimit.reserve(8388608);
            for (int i = 0; i < 63; ++i) atLimit += std::string(131071, 'p') + '\n';
            atLimit += std::string(131072, 'p');
            CHECK(atLimit.size() == 8388608 && assessText(atLimit).limit == TextLimit::Within);
            deletionAtLimit(atLimit, QStringLiteral(
                "Text of 8388609 bytes exceeds the supported 8388608 byte recovery payload limit. "
                "The existing edit was kept and this change was not acknowledged."));
        });
        return results.report();
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir data;
    if (!data.isValid()) return 2;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (data.path() + "/config").toUtf8());
    qputenv("XDG_CACHE_HOME", (data.path() + "/cache").toUtf8());
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("BranchTruceComparisonTests");
    // No argument runs every case; "scenes" and "boundary" select one ctest entry's share.
    // "display" runs the viewport cases on whatever platform Qt was given, such as a real
    // Wayland session.
    const auto arguments = QCoreApplication::arguments();
    auto selection = Selection::All;
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("scenes")) selection = Selection::Scenes;
    else if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("boundary")) selection = Selection::Boundary;
    else if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("display")) selection = Selection::Display;
    else if (arguments.size() != 1) return 2;
    // Synthetic offscreen integration owns its own clipboard. Physical desktop
    // input and window-manager behavior remain a separate verification layer.
    // The display cases use neither the clipboard nor real input, and leave the
    // session's clipboard alone.
    if (selection == Selection::Display) return ComparisonViewTests{}.run(selection);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    const auto result = ComparisonViewTests{}.run(selection);
    QGuiApplication::clipboard()->clear();
    return result;
}
