#include "app/ApplicationController.h"
#include "app/PaneHighlighter.h"
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
    const FileSession &file() { return *controller.files()->file("app"); }

    ApplicationController controller;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

bool hasFormats(QTextDocument *document)
{
    for (auto block = document->begin(); block.isValid(); block = block.next())
        if (!block.layout()->formats().isEmpty()) return true;
    return false;
}

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
}

class SyntaxHighlightingTests {
public:
    int run()
    {
        test_support::Suite suite("SyntaxHighlightingTests");
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
            settle([&] {
                for (const auto *pane : panes)
                    if (hasFormats(fixture.document(pane))) return false;
                return true;
            });
            for (const auto *pane : panes) {
                auto *highlighter = fixture.highlighter(pane);
                CHECK(!highlighter->definition().isValid());
                CHECK(highlighter->document() == nullptr);
            }
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
