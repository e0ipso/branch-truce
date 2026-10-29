#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "highlighting/PaneHighlighter.h"
#include "storage/RecoveryStore.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/QuickScene.h"
#include <QApplication>
#include <QColor>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextLayout>
#include <QtQml/qqml.h>

using namespace branch_truce;

namespace {
using test_support::settle;

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
        suite.run("TRUCE-0114 app.py gets the Python definition in every pane and an unknown extension gets none", [] {
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
        suite.run("TRUCE-0114 a format-only highlight pass creates no revision and no recovery transaction", [] {
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
            auto *comparison = fixture.item("comparisonPanes")->parentItem();
            QQmlExpression dark(qmlContext(fixture.item("comparisonPanes")), comparison,
                QStringLiteral("Kirigami.Theme.backgroundColor = \"#202326\""));
            dark.evaluate();
            if (dark.hasError()) throw std::runtime_error(dark.error().toString().toStdString());
            settle([&] { return fixture.highlighter("resultText")->theme().name() == QStringLiteral("Breeze Dark"); });
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
        suite.run("TRUCE-0119 changed words are tinted and underlined in every pane, and a format pass makes no revision", [] {
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
        suite.run("TRUCE-0173 a .txt file keeps one highlighter attached to every pane", [] {
            Fixture fixture;
            CHECK(fixture.controller.selectFile("notes"));
            settlePlain(fixture);
        });
        suite.run("TRUCE-0173 a decoration pass creates no revision and no recovery transaction", [] {
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
