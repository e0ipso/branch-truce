#include "app/ApplicationController.h"
#include "app/RecoveryRestore.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtQml/qqml.h>

using namespace branch_truce;

namespace {
template<typename Predicate>
void settle(Predicate ready)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(ready());
}

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
                CHECK(label.startsWith(side == SourceSide::Stage2 ? "Left source\n" : "Right source\n"));
                CHECK(label.contains(QString::fromStdString(source->label)));
                CHECK(fixture.item((std::string(prefix) + "Identity").c_str())->property("text").toString().contains(QString::fromStdString(source->objectId)));
                const auto projected = fixture.controller.selectedFile().value(prefix).toMap();
                CHECK(projected.value("commitId").toString().toStdString() == *source->commitId);
            }
            CHECK(!fixture.item("resultText")->property("readOnly").toBool());
            CHECK(fixture.item("resultLabel")->property("text").toString().startsWith("Editable result\n"));
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
            auto *pane = fixture.item("stage2Label")->parentItem();
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
