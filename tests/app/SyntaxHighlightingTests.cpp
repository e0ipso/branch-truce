#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "highlighting/PaneHighlighter.h"
#include "storage/RecoveryStore.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/QuickScene.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QtQml/qqml.h>

using namespace branch_truce;

namespace {
using test_support::settle;
using test_support::settleFor;

const char *const panes[] = {"stage2Text", "resultText", "stage3Text", "earlierText"};

// Python in every pane, each with a comment, a keyword and a string.
FileSession pythonFile(const std::string &id, const std::string &path)
{
    const std::string earlier = "# Greeting\ndef greet(name):\n    return 'hello ' + name\n";
    auto sources = *test_support::choiceFile("# Left greeting\ndef greet(name):\n    return 'hi ' + name\n",
        "# Right greeting\ndef greet(name):\n    return 'hey ' + name\n", earlier).snapshots();
    sources.path = path;
    sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
        test_support::choiceText(earlier), "earlier-commit", "Common earlier version"};
    return FileSession(id, path, {earlier, {}, 0}, Support::Supported, {}, std::move(sources), true);
}

class Fixture {
public:
    Fixture()
    {
        CHECK(controller.files()->appendFile(pythonFile("app", "src/app.py")));
        CHECK(controller.files()->appendFile(pythonFile("unknown", "src/app.unknownext")));
        CHECK(controller.files()->appendFile(pythonFile("notes", "docs/notes.txt")));
        CHECK(controller.selectFile("app"));
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
        if (!found) throw std::runtime_error(std::string("missing comparison item: ") + name);
        return found;
    }
    QTextDocument *document(const char *pane) const
    {
        auto *quick = item(pane)->property("textDocument").value<QQuickTextDocument *>();
        CHECK(quick && quick->textDocument());
        return quick->textDocument();
    }
    // Exactly one highlighter per pane document.
    PaneHighlighter *highlighter(const char *pane) const
    {
        const auto all = document(pane)->findChildren<QSyntaxHighlighter *>();
        CHECK(all.size() == 1);
        auto *found = qobject_cast<PaneHighlighter *>(all.front());
        CHECK(found);
        return found;
    }
    qsizetype transactions()
    {
        return storage::RecoveryStore(controller.files()->recoveryDirectory()).read().transactions.size();
    }
    const FileSession &file(const std::string &id = "app") { return *controller.files()->file(id); }

    ApplicationController controller{gitOnPath()};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

// Syntax formats set a foreground; word-difference decorations never do.
bool hasFormats(QTextDocument *document)
{
    for (auto block = document->begin(); block.isValid(); block = block.next())
        for (const auto &range : block.layout()->formats())
            if (range.format.hasProperty(QTextFormat::ForegroundBrush)) return true;
    return false;
}

// The format drawn at a document position, or an empty one.
QTextCharFormat formatAt(QTextDocument *document, int position)
{
    const auto block = document->findBlock(position);
    for (const auto &range : block.layout()->formats())
        if (range.start <= position - block.position() && position - block.position() < range.start + range.length)
            return range.format;
    return {};
}

// Whether every character of word, found after the first occurrence of
// context, is drawn underlined.
bool underlined(QTextDocument *document, const QString &context, const QString &word)
{
    const auto text = document->toPlainText();
    const auto from = text.indexOf(context);
    const auto at = from < 0 ? -1 : text.indexOf(word, from);
    if (at < 0) return false;
    for (auto position = at; position < at + word.size(); ++position)
        if (formatAt(document, static_cast<int>(position)).underlineStyle() != QTextCharFormat::SingleUnderline)
            return false;
    return true;
}

bool undecorated(QTextDocument *document, const QString &word)
{
    const auto at = document->toPlainText().indexOf(word);
    if (at < 0) return false;
    for (auto position = at; position < at + word.size(); ++position)
        if (formatAt(document, static_cast<int>(position)).underlineStyle() != QTextCharFormat::NoUnderline)
            return false;
    return true;
}

#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
// The foreground given to the first character of the first occurrence of
// word, or an invalid colour when no format covers it.
QColor foreground(QTextDocument *document, const QString &word)
{
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const auto at = block.text().indexOf(word);
        if (at < 0) continue;
        for (const auto &range : block.layout()->formats())
            if (range.start <= at && at < range.start + range.length) return range.format.foreground().color();
        return {};
    }
    return {};
}

// Comments, keywords and strings each get their own colour.
void checkPythonColours(QTextDocument *document)
{
    const auto comment = foreground(document, QStringLiteral("#"));
    const auto keyword = foreground(document, QStringLiteral("def"));
    const auto string = foreground(document, QStringLiteral("'h"));
    CHECK(comment.isValid() && keyword.isValid() && string.isValid());
    CHECK(comment != keyword && keyword != string && string != comment);
}

void settleHighlighted(Fixture &fixture)
{
    settle([&] {
        for (const auto *pane : panes)
            if (!hasFormats(fixture.document(pane))) return false;
        return true;
    });
}
#endif

// Every pane has its highlighter attached and no syntax formats.
void settlePlain(Fixture &fixture)
{
    settle([&] {
        for (const auto *pane : panes)
            if (hasFormats(fixture.document(pane))) return false;
        return true;
    });
    for (const auto *pane : panes) {
        auto *highlighter = fixture.highlighter(pane);
        CHECK(highlighter->document() == fixture.document(pane));
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
        CHECK(!highlighter->definition().isValid());
#endif
    }
}
}

class SyntaxHighlightingTests {
public:
    int run()
    {
        test_support::Suite suite("SyntaxHighlightingTests");
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
        suite.run("app.py gets the Python definition in every pane and an unknown extension gets none", [] {
            Fixture fixture;
            settleHighlighted(fixture);
            for (const auto *pane : panes) {
                auto *highlighter = fixture.highlighter(pane);
                CHECK(highlighter->definition().name() == QStringLiteral("Python"));
                CHECK(highlighter->document() == fixture.document(pane));
                CHECK(highlighter->theme().name() == QStringLiteral("Breeze Light"));
                checkPythonColours(fixture.document(pane));
            }
            CHECK(fixture.controller.selectFile("unknown"));
            settlePlain(fixture);
            CHECK(fixture.controller.selectFile("app"));
            settleHighlighted(fixture);
            for (const auto *pane : panes)
                CHECK(fixture.highlighter(pane)->definition().name() == QStringLiteral("Python"));
        });
        suite.run("a format-only highlight pass creates no revision and no recovery transaction", [] {
            Fixture fixture;
            settleHighlighted(fixture);
            auto *result = fixture.document("resultText");
            const auto before = fixture.file().current();
            const auto canUndo = fixture.file().canUndo();
            const auto text = result->toPlainText();
            const auto transactions = fixture.transactions();
            CHECK(transactions > 0);
            const auto unchanged = [&] {
                CHECK(fixture.file().current().id == before.id);
                CHECK(fixture.file().current().draft == before.draft);
                CHECK(fixture.file().canUndo() == canUndo);
                CHECK(fixture.transactions() == transactions);
                CHECK(!fixture.controller.files()->hasPendingRecovery());
                CHECK(result->toPlainText() == text);
            };
            const auto lightKeyword = foreground(result, QStringLiteral("def"));
            fixture.highlighter("resultText")->rehighlight();
            CHECK(foreground(result, QStringLiteral("def")) == lightKeyword);
            unchanged();
            // A dark scheme background reaches the controller through the
            // Kirigami.Theme binding and recolours every pane.
            // The SplitView is inside a layout Item owned by ComparisonView.
            auto *comparison = fixture.item("comparisonPanes")->parentItem()->parentItem();
            CHECK(comparison && comparison->property("schemeBackground").isValid());
            QQmlExpression dark(qmlContext(comparison), comparison,
                QStringLiteral("Kirigami.Theme.backgroundColor = \"#202326\""));
            dark.evaluate();
            if (dark.hasError()) throw std::runtime_error(dark.error().toString().toStdString());
            CHECK(comparison->property("schemeBackground").value<QColor>() == QColor(QStringLiteral("#202326")));
            settleFor("dark syntax theme", [&] { return fixture.highlighter("resultText")->theme().name() == QStringLiteral("Breeze Dark"); });
            for (const auto *pane : panes) {
                CHECK(fixture.highlighter(pane)->theme().name() == QStringLiteral("Breeze Dark"));
                checkPythonColours(fixture.document(pane));
            }
            CHECK(foreground(result, QStringLiteral("def")) != lightKeyword);
            unchanged();
            fixture.controller.setSyntaxBackground(QColor(QStringLiteral("#eff0f1")));
            CHECK(foreground(result, QStringLiteral("def")) == lightKeyword);
            unchanged();
            // A real edit is still one revision and one transaction, and stays highlighted.
            auto *editor = fixture.item("resultText");
            CHECK(!editor->property("readOnly").toBool());
            editor->forceActiveFocus();
            CHECK(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 0)));
            QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            QCoreApplication::sendEvent(editor, &key);
            settle([&] { return fixture.file().current().id != before.id; });
            CHECK(fixture.file().current().draft.text == "x" + before.draft.text);
            CHECK(fixture.transactions() == transactions + 1);
            CHECK(result->toPlainText() == QStringLiteral("x") + text);
            checkPythonColours(result);
        });
#endif
        suite.run("changed words are tinted and underlined in every pane, and a format pass makes no revision", [] {
            Fixture fixture;
            auto *result = fixture.document("resultText");
            auto *left = fixture.document("stage2Text");
            auto *right = fixture.document("stage3Text");
            // The competing sides pair with each other; the result pairs with both sides.
            const auto decorated = [&](const QString &prefix) {
                return underlined(left, QStringLiteral("# Left"), QStringLiteral("Left"))
                    && underlined(left, QStringLiteral("'hi"), QStringLiteral("hi"))
                    && underlined(right, QStringLiteral("# Right"), QStringLiteral("Right"))
                    && underlined(right, QStringLiteral("'hey"), QStringLiteral("hey"))
                    && underlined(result, prefix + QStringLiteral("'hello"), QStringLiteral("hello"));
            };
            settle([&] { return decorated({}); });
            for (auto *document : {left, right, result}) CHECK(undecorated(document, QStringLiteral("return")));
            const auto hello = result->toPlainText().indexOf(QStringLiteral("hello"));
            const auto tint = formatAt(result, static_cast<int>(hello));
            CHECK(tint.background().color().alpha() > 0 && tint.background().color().alpha() < 255);
            CHECK(tint.underlineColor().isValid() && tint.underlineColor().alpha() == 255);
            // The quote before the word is unchanged and stays undecorated.
            CHECK(formatAt(result, static_cast<int>(hello) - 1).underlineStyle() == QTextCharFormat::NoUnderline);
#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
            settleHighlighted(fixture);
            // The string colour stays under the decoration.
            CHECK(formatAt(result, static_cast<int>(hello)).hasProperty(QTextFormat::ForegroundBrush));
            checkPythonColours(result);
#endif
            const auto before = fixture.file().current();
            const auto canUndo = fixture.file().canUndo();
            const auto text = result->toPlainText();
            const auto transactions = fixture.transactions();
            CHECK(transactions > 0);
            const auto unchanged = [&] {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                CHECK(fixture.file().current().id == before.id);
                CHECK(fixture.file().current().draft == before.draft);
                CHECK(fixture.file().canUndo() == canUndo);
                CHECK(fixture.transactions() == transactions);
                CHECK(!fixture.controller.files()->hasPendingRecovery());
                CHECK(result->toPlainText() == text);
                CHECK(decorated({}));
            };
            for (const auto *pane : panes) fixture.highlighter(pane)->rehighlight();
            unchanged();
            fixture.controller.setSyntaxBackground(QColor(QStringLiteral("#202326")));
            unchanged();
            fixture.controller.setSyntaxBackground(QColor(QStringLiteral("#eff0f1")));
            unchanged();
            // An edit moves the ranges with the text of its revision.
            auto *editor = fixture.item("resultText");
            editor->forceActiveFocus();
            CHECK(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 0), Q_ARG(int, 0)));
            QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
            QCoreApplication::sendEvent(editor, &key);
            settle([&] { return fixture.file().current().id != before.id; });
            CHECK(fixture.transactions() == transactions + 1);
            settle([&] { return decorated(QStringLiteral("return ")); });
            CHECK(formatAt(result, static_cast<int>(hello)).underlineStyle() == QTextCharFormat::NoUnderline);
            CHECK(undecorated(result, QStringLiteral("return")));
        });
        suite.run("a .txt file keeps one highlighter attached to every pane", [] {
            Fixture fixture;
            CHECK(fixture.controller.selectFile("notes"));
            settlePlain(fixture);
        });
        suite.run("scattered decorations publish one document change without changing text or history", [] {
            QTextDocument document;
            document.setPlainText(QStringLiteral("plain text line\n").repeated(32));
            PaneHighlighter highlighter(&document);
            highlighter.attach();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            document.setModified(false);
            const auto text = document.toPlainText();
            const auto undo = document.isUndoAvailable();
            const auto redo = document.isRedoAvailable();
            QTextCursor selection(&document);
            selection.setPosition(3);
            selection.setPosition(8, QTextCursor::KeepAnchor);
            int notifications = 0;
            const auto connection = QObject::connect(&document, &QTextDocument::contentsChanged,
                &document, [&] { ++notifications; });
            QList<QTextLayout::FormatRange> ranges;
            for (const int line : {2, 7, 12}) {
                QTextLayout::FormatRange range;
                range.start = document.findBlockByNumber(line).position();
                range.length = 5;
                range.format.setFontUnderline(true);
                ranges.append(range);
            }
            highlighter.setDecorations(ranges);
            CHECK(notifications == 1);
            for (const auto &range : ranges) {
                const auto formats = document.findBlock(range.start).layout()->formats();
                CHECK(!formats.isEmpty() && formats.front().format.fontUnderline());
            }
            highlighter.setDecorations(ranges);
            CHECK(notifications == 1);
            notifications = 0;
            highlighter.setDecorations({});
            CHECK(notifications == 1);
            for (const auto &range : ranges)
                CHECK(document.findBlock(range.start).layout()->formats().isEmpty());
            CHECK(document.toPlainText() == text);
            CHECK(!document.isModified());
            CHECK(document.isUndoAvailable() == undo && document.isRedoAvailable() == redo);
            CHECK(selection.anchor() == 3 && selection.position() == 8);
            QObject::disconnect(connection);
        });
        suite.run("a decoration pass creates no revision and no recovery transaction", [] {
            Fixture fixture;
            CHECK(fixture.controller.selectFile("notes"));
            settlePlain(fixture);
            auto *result = fixture.document("resultText");
            const auto before = fixture.file("notes").current();
            const auto canUndo = fixture.file("notes").canUndo();
            const auto text = result->toPlainText();
            const auto transactions = fixture.transactions();
            CHECK(transactions > 0);
            QTextLayout::FormatRange range;
            range.start = 2;
            range.length = 4;
            range.format.setFontUnderline(true);
            auto *highlighter = fixture.highlighter("resultText");
            highlighter->setDecorations({range});
            CHECK(highlighter->decorations() == QList<QTextLayout::FormatRange>{range});
            highlighter->setDecorations({});
            CHECK(highlighter->decorations().isEmpty());
            // Let a queued edit capture run, if the pass started one.
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            CHECK(fixture.file("notes").current().id == before.id);
            CHECK(fixture.file("notes").current().draft == before.draft);
            CHECK(fixture.file("notes").canUndo() == canUndo);
            CHECK(fixture.transactions() == transactions);
            CHECK(!fixture.controller.files()->hasPendingRecovery());
            CHECK(result->toPlainText() == text);
            CHECK(highlighter->document() == result);
        });
        suite.run("edits before decorated blocks keep their local formats without another layout pass", [] {
            QTextDocument document;
            document.setPlainText(QStringLiteral("plain text line\n").repeated(32));
            PaneHighlighter highlighter(&document);
            highlighter.attach();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QList<QTextBlock> decorated{document.findBlockByNumber(4), document.findBlockByNumber(9),
                                             document.findBlockByNumber(14)};
            const auto ranges = [&] {
                QList<QTextLayout::FormatRange> found;
                for (const auto &block : decorated) {
                    QTextLayout::FormatRange range;
                    range.start = block.position() + 2;
                    range.length = 5;
                    range.format.setFontUnderline(true);
                    found.append(range);
                }
                return found;
            };
            highlighter.setDecorations(ranges());
            QTextCursor selection(&document);
            selection.setPosition(decorated.last().position() + 1);
            selection.setPosition(selection.position() + 3, QTextCursor::KeepAnchor);
            int notifications = 0;
            int updates = 0;
            const auto changed = QObject::connect(&document, &QTextDocument::contentsChanged,
                &document, [&] { ++notifications; });
            const auto laidOut = QObject::connect(document.documentLayout(), &QAbstractTextDocumentLayout::update,
                &document, [&](const QRectF &) { ++updates; });
            const auto formatState = [](const QList<QTextLayout::FormatRange> &formats) {
                QJsonArray state;
                for (const auto &range : formats) {
                    QJsonArray properties;
                    const auto values = range.format.properties();
                    for (auto it = values.cbegin(); it != values.cend(); ++it)
                        properties.append(QJsonObject{{"property", it.key()}, {"value", QJsonValue::fromVariant(it.value())}});
                    state.append(QJsonObject{{"start", range.start}, {"length", range.length},
                        {"underline", range.format.fontUnderline()}, {"underlineStyle", static_cast<int>(range.format.underlineStyle())},
                        {"properties", properties}});
                }
                return state;
            };
            const auto blockState = [&] {
                QJsonArray state;
                for (const auto &block : decorated) {
                    if (!block.isValid()) {
                        state.append(QJsonObject{{"valid", false}});
                        continue;
                    }
                    state.append(QJsonObject{{"valid", block.isValid()}, {"number", block.blockNumber()},
                        {"position", block.position()}, {"revision", block.revision()}, {"text", block.text()},
                        {"currentIdentity", document.findBlockByNumber(block.blockNumber()) == block},
                        {"formats", formatState(block.layout()->formats())}});
                }
                return state;
            };
            for (const auto &prefix : {QStringLiteral("prefix "), QStringLiteral("new line\n")}) {
                const auto beforeEdit = blockState();
                const auto previousRanges = formatState(highlighter.decorations());
                QTextCursor edit(&document);
                edit.insertText(prefix);
                const auto text = document.toPlainText();
                const auto revision = document.revision();
                const auto modified = document.isModified();
                const auto undo = document.isUndoAvailable(), redo = document.isRedoAvailable();
                const int anchor = selection.anchor(), position = selection.position();
                const auto afterEdit = blockState();
                const auto wanted = ranges();
                const int nativeNotifications = notifications, nativeUpdates = updates;
                notifications = updates = 0;
                highlighter.setDecorations(wanted);
                if (notifications != 0 || updates != 0) {
                    const QJsonObject diagnostic{{"qt", qVersion()}, {"prefix", prefix},
                        {"notifications", notifications}, {"layoutUpdates", updates},
                        {"notificationsBeforeRangeUpdate", nativeNotifications}, {"layoutUpdatesBeforeRangeUpdate", nativeUpdates},
                        {"documentRevisionBeforeRangeUpdate", revision}, {"documentRevisionAfterRangeUpdate", document.revision()},
                        {"beforeEdit", beforeEdit}, {"afterEdit", afterEdit}, {"afterRangeUpdate", blockState()},
                        {"previousRanges", previousRanges}, {"wantedRanges", formatState(wanted)}};
                    std::cerr << "DECORATION_SHIFT_DIAGNOSTIC="
                              << QJsonDocument(diagnostic).toJson(QJsonDocument::Compact).constData() << '\n';
                }
                CHECK(notifications == 0 && updates == 0);
                CHECK(document.revision() == revision);
                CHECK(document.toPlainText() == text && document.isModified() == modified);
                CHECK(document.isUndoAvailable() == undo && document.isRedoAvailable() == redo);
                CHECK(selection.anchor() == anchor && selection.position() == position);
                for (const auto &range : ranges()) {
                    for (int at = range.start; at < range.start + range.length; ++at)
                        CHECK(formatAt(&document, at).fontUnderline());
                    CHECK(!formatAt(&document, range.start - 1).fontUnderline());
                    CHECK(!formatAt(&document, range.start + range.length).fontUnderline());
                }
            }
            highlighter.setDecorations({});
            for (const auto &block : decorated) CHECK(block.layout()->formats().isEmpty());
            QObject::disconnect(changed);
            QObject::disconnect(laidOut);
        });
        suite.run("local overlays follow replacement splitting deletion and search precedence", [] {
            QTextDocument document;
            document.setPlainText(QStringLiteral("plain text line\n").repeated(32));
            PaneHighlighter highlighter(&document);
            highlighter.attach();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            auto block = document.findBlockByNumber(4);
            QTextLayout::FormatRange word;
            word.start = block.position() + 2;
            word.length = 7;
            word.format.setFontUnderline(true);
            word.format.setBackground(QColor(Qt::yellow));
            auto search = word;
            search.length = 2;
            search.format = QTextCharFormat();
            search.format.setBackground(QColor(Qt::magenta));
            highlighter.setDecorations({word});
            highlighter.setSearchDecorations({search});
            CHECK(formatAt(&document, word.start).fontUnderline());
            CHECK(formatAt(&document, word.start).background().color() == QColor(Qt::magenta));
            CHECK(formatAt(&document, word.start + 3).background().color() == QColor(Qt::yellow));
            QTextCursor edit(&document);
            edit.setPosition(word.start + 2);
            edit.setPosition(word.start + 4, QTextCursor::KeepAnchor);
            edit.insertText(QStringLiteral("XY"));
            highlighter.setDecorations({word});
            CHECK(formatAt(&document, word.start + 3).fontUnderline());
            // Split the decorated range across two new layouts. The inserted
            // paragraph separator itself has no character overlay.
            edit.setPosition(word.start + 3);
            edit.insertText(QStringLiteral("\n"));
            ++word.length;
            highlighter.setDecorations({word});
            for (int at = word.start; at < word.start + word.length; ++at) {
                if (document.characterAt(at) == QChar::ParagraphSeparator) continue;
                CHECK(formatAt(&document, at).fontUnderline());
            }
            CHECK(formatAt(&document, search.start).background().color() == QColor(Qt::magenta));
            highlighter.setSearchDecorations({});
            CHECK(formatAt(&document, search.start).background().color() == QColor(Qt::yellow));
            // Delete both pieces, including one cached live block. Any
            // formats temporarily applied at the old offsets must be cleared.
            const int first = block.position();
            const auto after = block.next().next();
            CHECK(after.isValid());
            edit.setPosition(first);
            edit.setPosition(after.position(), QTextCursor::KeepAnchor);
            edit.removeSelectedText();
            highlighter.setDecorations({});
            for (auto current = document.begin(); current.isValid(); current = current.next())
                CHECK(current.layout()->formats().isEmpty());
            document.setPlainText(QStringLiteral("replacement text\n").repeated(32));
            word.start = document.findBlockByNumber(8).position() + 1;
            word.length = 4;
            highlighter.setDecorations({word});
            CHECK(formatAt(&document, word.start).fontUnderline());
            highlighter.setDecorations({});
            for (auto current = document.begin(); current.isValid(); current = current.next())
                CHECK(current.layout()->formats().isEmpty());
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
    QCoreApplication::setApplicationName("BranchTruceSyntaxHighlightingTests");
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    return SyntaxHighlightingTests{}.run();
}
