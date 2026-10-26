#include "app/ApplicationController.h"
#include "app/RecoveryRestore.h"
#include "core/ResourceLimits.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/QuickScene.h"
#include "fixtures/RecoveryFaults.h"
#include <QApplication>
#include <algorithm>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMap>
#include <QSet>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtQml/qqml.h>

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
    // Open a toolbar menu and click one of its entries once the menu has laid out.
    void clickMenuEntry(const char *menu, const char *entry)
    {
        click(menu);
        test_support::clickItem(window, test_support::waitForLayout(window, QString::fromLatin1(entry), 60));
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

    ApplicationController controller;
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
}

class ComparisonViewTests {
public:
    int run()
    {
        test_support::Suite suite("ComparisonViewTests");
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
            CHECK(!fixture.item("scopedChoices")->isVisible());
            CHECK(fixture.item("showSourceChoices")->setProperty("checked", true));
            settle([&] { return fixture.item("scopedChoices")->isVisible(); });
            CHECK(fixture.item("showSourceChoices")->setProperty("checked", false));
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
        suite.run("TRUCE-0113 every coloured line has a gutter glyph of its kind", [] {
            Fixture fixture(test_support::choiceFile("header\nleft first\nshared\ntail\n",
                "header\nright first\nshared\nright extra\ntail\n"));
            const QMap<QString, QString> glyphs{{"added", "+"}, {"changed", "~"}, {"conflict", "!"},
                {"manual", "m"}, {"removed", QString(QChar(0x2212))}};
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
            const auto revision = [&] { return QString::number(fixture.file().current().id); };
            CHECK(fixture.controller.previewWholeFile("choices", revision(), "left"));
            CHECK(fixture.controller.applyWholeFile());
            const auto at = static_cast<int>(fixture.text().indexOf("left first"));
            CHECK(fixture.controller.editResult("choices", revision(), at, at + 4, "LEFT"));
            expect("stage2", {"changed"});
            expect("result", {"manual", "removed"});
            expect("stage3", {"added", "changed"});
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
        suite.run("TRUCE-0137 undo and redo keep the result cursor and scroll position, and the sources stay", [] {
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
        });
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
            settle([&] {
                return fixture.item("confirmResolution")->isVisible() && fixture.item("cancelResolution")->isVisible()
                    && fixture.item("cancelResolution")->width() > 0 && fixture.item("cancelResolution")->height() > 0;
            });
            fixture.click("cancelResolution");
            CHECK(!fixture.controller.resolutionConfirmation());
            CHECK(fixture.controller.commandMessage().contains(QStringLiteral("unchanged")));
            CHECK(fixture.text() == text && fixture.file().current().draft.text == text.toStdString());
            CHECK(!fixture.controller.markResolved(QStringLiteral("choices"), QStringLiteral("999"), true));
            CHECK(fixture.controller.commandMessage().contains(QStringLiteral("changed")));
            CHECK(fixture.text() == text && !fixture.file().reviewed() && !fixture.file().facts().resolved);
            fixture.click("markResolved");
            settle([&] { return fixture.item("confirmResolution")->isVisible() && fixture.item("confirmResolution")->height() > 0; });
            fixture.click("confirmResolution");
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
        suite.run("TRUCE-0063 Backspace and Delete shorten an exact 262144-byte line", [] {
            deletionAtLimit(std::string(262144, 'a'), QStringLiteral(
                "Line of 262145 bytes exceeds the supported 262144 byte line limit. "
                "The existing edit was kept and this change was not acknowledged."));
        });
        suite.run("TRUCE-0063 Backspace and Delete shorten an exact 8388608-byte buffer", [] {
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
        return suite.report();
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
    // Synthetic offscreen integration owns its own clipboard. Physical desktop
    // input and window-manager behavior remain a separate verification layer.
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    const auto result = ComparisonViewTests{}.run();
    QGuiApplication::clipboard()->clear();
    return result;
}
