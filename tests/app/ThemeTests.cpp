// The real Main scene under the app's own look: its icon names, its colour roles in a dark and
// a light KDE colour scheme, a non-colour cue for every coloured line, and what typing and
// scrolling cost once the panes are highlighted.
//
//   branch_truce_theme_tests icons <icon list>   every collected icon.name resolves
//   branch_truce_theme_tests roles <scheme>      the sidebar and panes paint the scheme's View background
//   branch_truce_theme_tests cues                every coloured line has a gutter glyph
//   branch_truce_theme_tests cost                typing and scrolling stay inside their limits
//
// Each mode sets up the style and icon themes as src/app/main.cpp does, so the scene looks as
// it does in the app. A check that cannot apply on this host, such as the icon check without a
// Breeze icon theme, exits 77 with the reason, and ctest reports it as skipped.
//
// To see a check fail, give it a broken copy of the BranchTruce module. Copy the qmldir and
// qmltypes from build/desktop/src/ui/BranchTruce and the QML files of src/ui into <dir>/BranchTruce,
// delete the qmldir's "prefer" line so the copied files are used, break one of them, copy this
// executable into <dir> and run it there: the executable's directory comes first on the QML import
// path. For the icon check, run app/CollectIconNames.cmake over the QML files plus a fixture QML
// file that names a missing icon, and pass that list.
#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "highlighting/PaneHighlighter.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/QuickScene.h"
#include "fixtures/performance/TypingTrace.h"
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

using namespace branch_truce;

namespace {
constexpr int kSkipped = 77;

#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
// The limits docs/resource-limits.md records for the cost workload: the median CPU time, in
// milliseconds, that the process spends from one key press, or one wheel notch, to the next
// rendered frame. CPU time rather than wall time, so time other processes hold the CPU does not count.
constexpr qint64 kTypingLimitMs = 200;
constexpr qint64 kScrollLimitMs = 300;
// The workload: a C++ file as long as the gate's editor-lines-200 workload in
// performance/WorkloadMeasurements.cpp, with a conflict every kRegionEvery lines.
constexpr int kWorkloadLines = 200;
constexpr int kRegionEvery = 20;
constexpr int kRepeats = 3;
constexpr int kKeysPerRepeat = 6;
constexpr int kNotchesPerRepeat = 15;
#endif

// As src/app/main.cpp: the KDE style when it is installed, with Fusion for the controls it lacks,
// and otherwise Fusion. QT_QUICK_CONTROLS_STYLE wins.
void selectControlsStyle(const QQmlEngine &engine)
{
    if (!qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) return;
    const auto paths = engine.importPathList();
    const bool kdeStyle = std::any_of(paths.cbegin(), paths.cend(), [](const QString &path) {
        return QFileInfo::exists(path + QStringLiteral("/org/kde/desktop/qmldir"));
    });
    QQuickStyle::setStyle(kdeStyle ? QStringLiteral("org.kde.desktop") : QStringLiteral("Fusion"));
    if (kdeStyle) QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
}

// As src/app/main.cpp: Breeze as the fallback icon theme, and hicolor when the platform names none.
void selectIconThemes()
{
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
    if (QIcon::themeName().isEmpty()) QIcon::setThemeName(QStringLiteral("hicolor"));
}

class Scene {
public:
    explicit Scene(FileSession file)
    {
        const auto id = file.id();
        CHECK(controller.files()->appendFile(std::move(file)));
        CHECK(controller.selectFile(QString::fromStdString(id)));
        selectControlsStyle(engine);
        engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)}});
        engine.loadFromModule("BranchTruce", "Main");
        CHECK(engine.rootObjects().size() == 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        CHECK(window);
        window->resize(1500, 1100);
        window->show();
        test_support::settleFor("the result pane", [&] {
            auto *result = window->findChild<QQuickItem *>(QStringLiteral("resultText"));
            return result && result->width() > 0 && result->height() > 0;
        });
        for (const auto *name : {"stage2Viewport", "resultViewport", "stage3Viewport"})
            test_support::waitForLayout(window, QString::fromLatin1(name), 60);
    }
    QQuickItem *item(const QString &name) const
    {
        auto *found = window->findChild<QQuickItem *>(name);
        if (!found) found = test_support::visualItem(window->contentItem(), name);
        if (!found) throw std::runtime_error("missing scene item: " + name.toStdString());
        return found;
    }
    void showEarlier()
    {
        auto *box = item(QStringLiteral("showEarlier"));
        CHECK(box->isEnabled());
        box->setProperty("checked", true);
        test_support::waitForLayout(window, QStringLiteral("earlierViewport"), 60);
    }

    ApplicationController controller{gitOnPath()};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

std::string describe(const QColor &color)
{
    return "rgb(" + std::to_string(color.red()) + "," + std::to_string(color.green()) + ","
        + std::to_string(color.blue()) + ")";
}

// --- icons ---------------------------------------------------------------------------------

// The theme directories that hold a Breeze icon theme.
QStringList breezeThemes()
{
    QStringList found;
    for (const auto &path : QIcon::themeSearchPaths()) {
        if (QFileInfo::exists(path + QStringLiteral("/breeze/index.theme"))) found.append(path + QStringLiteral("/breeze"));
    }
    return found;
}

int checkIcons(const QString &listPath)
{
    QFile list(listPath);
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "FAIL icons: cannot read the icon list " << listPath.toStdString() << '\n';
        return 1;
    }
    // name -> the places that use it
    std::map<QString, QStringList> names;
    while (!list.atEnd()) {
        const auto line = QString::fromUtf8(list.readLine()).trimmed();
        if (line.isEmpty()) continue;
        const auto fields = line.split(QLatin1Char('\t'));
        names[fields.front()].append(fields.size() > 1 ? fields.at(1) : QStringLiteral("unknown place"));
    }
    selectIconThemes();
    // Load the scene first: Kirigami's controls plugin can replace the icon theme and its search
    // path when it loads.
    Scene scene(test_support::batchFile());
    const auto themes = breezeThemes();
    if (themes.isEmpty()) {
        std::cout << "SKIP icons: no Breeze icon theme (breeze/index.theme) under "
                  << QIcon::themeSearchPaths().join(QStringLiteral(", ")).toStdString()
                  << "; install breeze-icon-theme to run this check\n";
        return kSkipped;
    }
    test_support::Suite suite("ThemeTests icons");
    suite.run("the icon list names icons", [&] { CHECK(!names.empty()); });
    std::cout << "icon theme " << QIcon::themeName().toStdString() << ", fallback "
              << QIcon::fallbackThemeName().toStdString() << ", Breeze at " << themes.join(", ").toStdString()
              << ", " << names.size() << " names\n";
    for (const auto &[name, places] : names) {
        suite.run("icon " + name.toStdString() + " resolves (" + places.join(QStringLiteral(", ")).toStdString() + ")",
            [&] { CHECK(QIcon::hasThemeIcon(name)); });
    }
    return suite.report();
}

// --- roles ---------------------------------------------------------------------------------

// The colour a KDE colour scheme file gives one key of one colour group, such as
// Colors:View/BackgroundNormal.
std::optional<QColor> schemeColour(const QString &path, const QString &group, const QString &key)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return std::nullopt;
    QString current;
    while (!file.atEnd()) {
        const auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            current = line.mid(1, line.size() - 2);
        } else if (current == group && line.startsWith(key + QLatin1Char('='))) {
            const auto parts = line.mid(key.size() + 1).split(QLatin1Char(','));
            if (parts.size() < 3) return std::nullopt;
            return QColor(parts.at(0).toInt(), parts.at(1).toInt(), parts.at(2).toInt());
        }
    }
    return std::nullopt;
}

QString schemeFile(const QString &scheme)
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                  QStringLiteral("color-schemes/") + scheme + QStringLiteral(".colors"));
}

// What the sidebar or a pane paints near the bottom of an item that has room below its content.
QColor sampleBelowContent(const QImage &frame, QQuickItem *item)
{
    const auto point = item->mapToScene(QPointF(item->width() * 0.6, item->height() - 8)).toPoint();
    if (!frame.rect().contains(point)) throw std::runtime_error("sample point outside the window");
    return frame.pixelColor(point);
}

bool near(const QColor &actual, const QColor &expected)
{
    return std::abs(actual.red() - expected.red()) <= 1 && std::abs(actual.green() - expected.green()) <= 1
        && std::abs(actual.blue() - expected.blue()) <= 1;
}

int checkRoles(const QString &scheme, const QString &other)
{
    const auto path = schemeFile(scheme);
    const auto otherPath = schemeFile(other);
    if (path.isEmpty() || otherPath.isEmpty()) {
        std::cout << "SKIP roles: no color-schemes/" << scheme.toStdString() << ".colors or "
                  << other.toStdString() << ".colors in the data directories; install the Breeze colour schemes\n";
        return kSkipped;
    }
    selectIconThemes();
    const auto suiteName = "ThemeTests roles " + scheme.toStdString();
    test_support::Suite suite(suiteName.c_str());
    const auto view = schemeColour(path, QStringLiteral("Colors:View"), QStringLiteral("BackgroundNormal"));
    const auto otherView = schemeColour(otherPath, QStringLiteral("Colors:View"), QStringLiteral("BackgroundNormal"));
    suite.run("the schemes give different View backgrounds", [&] {
        CHECK(view && otherView);
        CHECK(!near(*view, *otherView));
    });
    if (!view || !otherView) return suite.report();
    Scene scene(test_support::batchFile());
    if (QQuickStyle::name() != QStringLiteral("org.kde.desktop")) {
        std::cout << "SKIP roles: the Qt Quick Controls style is " << QQuickStyle::name().toStdString()
                  << ", which does not read the KDE colour scheme; install qqc2-desktop-style\n";
        return kSkipped;
    }
    scene.showEarlier();
    // Let the gutters and backgrounds lay out before the frame is taken.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const auto frame = scene.window->grabWindow();
    std::cout << "scheme " << scheme.toStdString() << ": View background " << describe(*view) << ", "
              << other.toStdString() << " " << describe(*otherView) << '\n';
    suite.run("the sidebar paints the View background", [&] {
        auto *list = scene.item(QStringLiteral("fileList"));
        CHECK(list->property("contentHeight").toDouble() + 16 < list->height());
        const auto shown = sampleBelowContent(frame, list);
        std::cout << "sidebar " << describe(shown) << '\n';
        CHECK(near(shown, *view));
    });
    for (const auto *pane : {"stage2", "result", "stage3", "earlier"}) {
        suite.run(std::string("the ") + pane + " pane paints the View background", [&] {
            auto *viewport = scene.item(QString::fromLatin1(pane) + QStringLiteral("Viewport"));
            auto *text = scene.item(QString::fromLatin1(pane) + QStringLiteral("Text"));
            CHECK(text->property("contentHeight").toDouble() + 16 < viewport->height());
            const auto shown = sampleBelowContent(frame, viewport);
            std::cout << pane << " pane " << describe(shown) << '\n';
            CHECK(near(shown, *view));
        });
    }
    return suite.report();
}

// --- cues ----------------------------------------------------------------------------------

// The lines a pane's background layer colours.
std::set<int> colouredLines(QQuickItem *backgrounds)
{
    std::set<int> lines;
    auto *fills = backgrounds->findChild<QQuickItem *>(backgrounds->objectName() + QStringLiteral("Lines"));
    if (!fills) throw std::runtime_error("missing line backgrounds");
    for (auto *child : fills->childItems()) {
        if (!child->isVisible() || !child->property("line").isValid()) continue;
        if (child->width() <= 0 || child->height() <= 0 || child->opacity() <= 0) continue;
        lines.insert(child->property("line").toInt());
    }
    return lines;
}

// The lines the gutter's marker slot gives a visible, non-empty glyph or chip. The rule for a
// removed line sits between two lines and marks neither.
std::map<int, QString> glyphLines(QQuickItem *slot)
{
    std::map<int, QString> glyphs;
    for (auto *child : slot->childItems()) {
        if (!child->isVisible() || child->property("kind").toString() == QStringLiteral("removed")) continue;
        const auto text = child->property("text").toString();
        if (text.trimmed().isEmpty() || child->width() <= 0 || child->height() <= 0) continue;
        glyphs[child->property("line").toInt()] = text;
    }
    return glyphs;
}

int checkCues()
{
    selectIconThemes();
    test_support::Suite suite("ThemeTests cues");
    // A left-only change, a right-only change and a conflict.
    Scene scene(test_support::batchFile());
    scene.showEarlier();
    std::size_t total = 0;
    for (const auto *pane : {"stage2", "result", "stage3", "earlier"}) {
        suite.run(std::string("every coloured line of the ") + pane + " pane has a glyph", [&] {
            const auto name = QString::fromLatin1(pane);
            auto *backgrounds = scene.item(name + QStringLiteral("Backgrounds"));
            auto *gutter = scene.item(name + QStringLiteral("Gutter"));
            auto *slot = scene.item(gutter->objectName() + QStringLiteral("MarkerSlot"));
            test_support::settleFor(std::string("the ") + pane + " gutter rows",
                [&] { return gutter->property("rows").toList().size() > 0; });
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const auto lines = colouredLines(backgrounds);
            const auto glyphs = glyphLines(slot);
            std::ostringstream shown;
            for (const auto line : lines) {
                const auto glyph = glyphs.find(line);
                shown << ' ' << line << '=' << (glyph == glyphs.end() ? std::string("(none)") : glyph->second.toStdString());
            }
            std::cout << pane << " coloured lines:" << shown.str() << '\n';
            // The earlier version is context: it has no coloured lines of its own.
            if (name != QStringLiteral("earlier")) CHECK(!lines.empty());
            for (const auto line : lines) {
                if (!glyphs.contains(line))
                    throw std::runtime_error(std::string(pane) + " line " + std::to_string(line)
                        + " is coloured but its gutter glyph is empty");
            }
            total += lines.size();
        });
    }
    suite.run("the fixture colours lines in three regions", [&] { CHECK(total >= 3); });
    return suite.report();
}

// --- cost ----------------------------------------------------------------------------------

#ifdef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
// A C++ file with a function every 10 lines. Every kRegionEvery lines, one statement differs
// between the left side, the right side and the common earlier version.
std::string workloadSource(const char *side)
{
    std::string text;
    for (int line = 0; line < kWorkloadLines; ++line) {
        char buffer[160];
        if (line % 10 == 0)
            std::snprintf(buffer, sizeof buffer, "int step_%d(int total) {\n", line);
        else if (line % 10 == 9)
            std::snprintf(buffer, sizeof buffer, "}\n");
        else if (line % kRegionEvery == 5)
            std::snprintf(buffer, sizeof buffer, "    total += weigh(\"%s\", %d) * %s_factor; // %s choice\n", side,
                          line, side, side);
        else
            std::snprintf(buffer, sizeof buffer, "    total = mix(total, %d); // shared statement %d\n", line, line);
        text += buffer;
    }
    return text;
}

FileSession workloadFile()
{
    const auto earlier = workloadSource("base");
    auto sources = *test_support::choiceFile(workloadSource("left"), workloadSource("right"), earlier).snapshots();
    const std::string path = "src/workload.cpp";
    sources.path = path;
    sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
        test_support::choiceText(earlier), "earlier-commit", "Common earlier version"};
    return FileSession("workload", path, {earlier, {}, 0}, Support::Supported, {}, std::move(sources), true);
}

PaneHighlighter *highlighterOf(QQuickItem *editor)
{
    auto *quick = editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quick || !quick->textDocument()) return nullptr;
    const auto all = quick->textDocument()->findChildren<PaneHighlighter *>();
    return all.size() == 1 ? all.front() : nullptr;
}

qint64 processCpuMs()
{
    timespec now {};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return static_cast<qint64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// Milliseconds of wall time and of process CPU time.
struct Cost {
    qint64 wall = -1;
    qint64 cpu = -1;
};

// Runs act, then waits until done() holds and a frame has been rendered after it. Returns the
// cost from act to that frame, with wall -1 when it did not happen within ten seconds.
template<typename Act, typename Done>
Cost costToFrame(QQuickWindow *window, Act act, Done done)
{
    QEventLoop loop;
    QElapsedTimer clock;
    qint64 cpuStart = 0;
    Cost cost;
    const auto connection = QObject::connect(window, &QQuickWindow::frameSwapped, &loop, [&] {
        if (cost.wall < 0 && done()) {
            cost.wall = clock.elapsed();
            cost.cpu = processCpuMs() - cpuStart;
            loop.quit();
        }
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    clock.start();
    cpuStart = processCpuMs();
    act();
    window->requestUpdate();
    if (cost.wall < 0) loop.exec();
    QObject::disconnect(connection);
    return cost;
}

qint64 median(std::vector<qint64> values)
{
    std::sort(values.begin(), values.end());
    return values.empty() ? -1 : values[values.size() / 2];
}

std::string join(const std::vector<qint64> &values)
{
    std::string text;
    for (const auto value : values) text += (text.empty() ? "" : " ") + std::to_string(value);
    return text;
}

void writeTypingDiagnostic(QJsonObject record)
{
    record.insert("schema", 1);
    std::cout << "HIGHLIGHTED_TYPING_DIAGNOSTIC="
              << QJsonDocument(record).toJson(QJsonDocument::Compact).constData() << '\n';
}

// Replay only after all budget samples and assertions. The same live scene now
// contains the measured edits; report that state rather than claiming a pristine
// replay or comparing instrumented CPU time against the uninstrumented limit.
void diagnoseTyping(Scene &scene)
{
    auto *result = scene.item(QStringLiteral("resultText"));
    auto *viewport = scene.item(QStringLiteral("resultViewport"));
    auto *document = result->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    const auto file = [&] { return scene.controller.files()->file("workload"); };
    test_support::TypingTrace trace;
    const auto dispatched = trace.add(QStringLiteral("input.sendEvent.return"));
    const auto model = trace.add(QStringLiteral("session.dataChanged"));
    const auto editor = trace.add(QStringLiteral("controller.resultEditorChanged"));
    const auto selected = trace.add(QStringLiteral("controller.selectedFileChanged"));
    const auto projected = trace.add(QStringLiteral("selectedFile.statusChanged"));
    const auto comparison = trace.add(QStringLiteral("selectedFile.comparisonChanged"));
    const auto passages = trace.add(QStringLiteral("selectedFile.passagesChanged"));
    const auto handEdited = trace.add(QStringLiteral("selectedFile.handEditedLinesChanged"));
    const auto choices = trace.add(QStringLiteral("selectedFile.choicesChanged"));
    const auto regions = trace.add(QStringLiteral("selectedFile.regionsChanged"));
    const auto markerBlocks = trace.add(QStringLiteral("selectedFile.markerBlocksChanged"));
    const auto markerOrphans = trace.add(QStringLiteral("selectedFile.markerOrphansChanged"));
    const auto summary = trace.add(QStringLiteral("session.summaryChanged"));
    const auto frame = trace.add(QStringLiteral("input.acknowledgedFrame"));
    QObject::connect(scene.controller.files(), &QAbstractItemModel::dataChanged, &trace, [&] { trace.mark(model); });
    QObject::connect(&scene.controller, &ApplicationController::resultEditorChanged, &trace, [&] { trace.mark(editor); });
    QObject::connect(&scene.controller, &ApplicationController::selectedFileChanged, &trace, [&] { trace.mark(selected); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::statusChanged, &trace, [&] { trace.mark(projected); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::comparisonChanged, &trace, [&] { trace.mark(comparison); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::passagesChanged, &trace, [&] { trace.mark(passages); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::handEditedLinesChanged, &trace, [&] { trace.mark(handEdited); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::choicesChanged, &trace, [&] { trace.mark(choices); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::regionsChanged, &trace, [&] { trace.mark(regions); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::markerBlocksChanged, &trace, [&] { trace.mark(markerBlocks); });
    QObject::connect(scene.controller.selectedFile(), &SelectedFile::markerOrphansChanged, &trace, [&] { trace.mark(markerOrphans); });
    QObject::connect(scene.controller.files(), &SessionModel::summaryChanged, &trace, [&] { trace.mark(summary); });
    QJsonArray panes;
    for (const auto *name : {"stage2Text", "resultText", "stage3Text"}) {
        auto *item = scene.item(QString::fromLatin1(name));
        auto *text = item->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        auto *highlighter = highlighterOf(item);
        trace.watchDocument(QString::fromLatin1(name), text);
        panes.append(QJsonObject{{"pane", name}, {"characters", text->characterCount()},
            {"blocks", text->blockCount()}, {"definition", highlighter ? highlighter->definition().name() : QString()},
            {"decorations", highlighter ? static_cast<int>(highlighter->decorations().size()) : 0}});
    }
    writeTypingDiagnostic({{"kind", "context"}, {"sequence", "after_budget_same_scene"},
        {"budget_samples_instrumented", false}, {"diagnostic_times_include_observer_overhead", true},
        {"timing_semantics", "cumulative public signal delivery offsets; overlapping, not exclusive costs"},
        {"cpu_clock", "CLOCK_PROCESS_CPUTIME_ID"}, {"offset_unit", "us"},
        {"offset_origin", "before_key_event_construction"},
        {"render_observation", "frameSwapped delivered to GUI thread after revision and length acknowledgment"},
        {"qt_runtime", qVersion()}, {"qt_build", QT_VERSION_STR}, {"os", QSysInfo::prettyProductName()},
        {"architecture", QSysInfo::buildCpuArchitecture()}, {"platform", QGuiApplication::platformName()},
        {"controls_style", QQuickStyle::name()},
        {"graphics_api", static_cast<int>(scene.window->rendererInterface()->graphicsApi())},
        {"window_width", scene.window->width()}, {"window_height", scene.window->height()},
        {"workload_path", "src/workload.cpp"}, {"workload_source", "in_memory_workloadFile_fixture"},
        {"source_lines", kWorkloadLines}, {"conflict_every", kRegionEvery},
        {"repeats", kRepeats}, {"keys_per_repeat", kKeysPerRepeat}, {"diagnostic_scroll_notches", 0}, {"panes", panes},
        {"input_path", "src/app/ResultEditorAdapter.cpp"}, {"model_path", "src/app/SessionModel.cpp"},
        {"layout_path", "src/ui/TextBlocks.cpp"}, {"highlight_path", "src/highlighting/PaneHighlighter.cpp"}});
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        const auto position = document->findBlockByNumber(kWorkloadLines / 2 + 3 + repeat).position() + 4;
        result->forceActiveFocus();
        result->setProperty("cursorPosition", position);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int key = 0; key < kKeysPerRepeat; ++key) {
            const auto before = file()->current().id;
            const auto length = document->characterCount();
            QJsonObject record{{"kind", "key"}, {"repeat", repeat + 1}, {"key", key + 1},
                {"revision_before", QString::number(before)}, {"characters_before", length},
                {"document_revision_before", document->revision()},
                {"blocks_before", document->blockCount()}, {"cursor_before", result->property("cursorPosition").toInt()},
                {"viewport_y", viewport->property("contentY").toDouble()},
                {"viewport_height", viewport->height()}, {"content_height", viewport->property("contentHeight").toDouble()}};
            const auto cost = costToFrame(scene.window, [&] {
                trace.begin();
                QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
                QCoreApplication::sendEvent(result, &press);
                trace.mark(dispatched);
            }, [&] {
                if (file()->current().id == before || document->characterCount() != length + 1) return false;
                trace.mark(frame);
                return true;
            });
            trace.finish();
            const auto timing = trace.json();
            record.insert("trace", timing);
            record.insert("acknowledged_and_drawn", cost.wall >= 0);
            record.insert("to_frame_wall_ms", cost.wall);
            if (timing.value("cpu_clock_valid").toBool() && cost.wall >= 0)
                record.insert("to_frame_process_cpu_ms", cost.cpu);
            record.insert("revision_after", QString::number(file()->current().id));
            record.insert("characters_after", document->characterCount());
            record.insert("document_revision_after", document->revision());
            writeTypingDiagnostic(record);
            if (cost.wall < 0) throw std::runtime_error("diagnostic key was not acknowledged and drawn");
        }
    }
}
#endif

int checkCost()
{
#ifndef BRANCH_TRUCE_SYNTAX_HIGHLIGHTING
    std::cout << "SKIP cost: this build has BRANCH_TRUCE_SYNTAX_HIGHLIGHTING=OFF, so the panes are not highlighted\n";
    return kSkipped;
#else
    selectIconThemes();
    test_support::Suite suite("ThemeTests cost");
    QElapsedTimer loadClock;
    loadClock.start();
    Scene scene(workloadFile());
    auto *result = scene.item(QStringLiteral("resultText"));
    auto *viewport = scene.item(QStringLiteral("resultViewport"));
    auto *left = scene.item(QStringLiteral("stage2Viewport"));
    suite.run("the panes are highlighted as C++ with word differences", [&] {
        for (const auto *pane : {"stage2Text", "resultText", "stage3Text"}) {
            auto *highlighter = highlighterOf(scene.item(QString::fromLatin1(pane)));
            CHECK(highlighter);
            test_support::settleFor(std::string(pane) + " highlighting", [&] {
                return highlighter->definition().name() == QStringLiteral("C++") && !highlighter->decorations().isEmpty();
            });
        }
    });
    std::cout << "workload " << kWorkloadLines << " lines, conflict every " << kRegionEvery
              << " lines, loaded and highlighted in " << loadClock.elapsed() << " ms\n";
    const auto file = [&] { return scene.controller.files()->file("workload"); };
    std::vector<qint64> typingMedians;
    std::vector<qint64> scrollMedians;
    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        suite.run("repeat " + std::to_string(repeat + 1) + " types and scrolls", [&] {
            // Type into a line near the middle, after its indentation.
            auto *document = result->property("textDocument").value<QQuickTextDocument *>()->textDocument();
            const auto position = document->findBlockByNumber(kWorkloadLines / 2 + 3 + repeat).position() + 4;
            result->forceActiveFocus();
            result->setProperty("cursorPosition", position);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            std::vector<qint64> keys;
            std::vector<qint64> keysWall;
            for (int key = 0; key < kKeysPerRepeat; ++key) {
                const auto before = file()->current().id;
                const auto length = document->characterCount();
                const auto cost = costToFrame(scene.window, [&] {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
                    QCoreApplication::sendEvent(result, &press);
                }, [&] { return file()->current().id != before && document->characterCount() == length + 1; });
                if (cost.wall < 0) throw std::runtime_error("key " + std::to_string(key) + " was not acknowledged and drawn");
                keys.push_back(cost.cpu);
                keysWall.push_back(cost.wall);
            }
            const auto inserted = document->toPlainText().mid(position, kKeysPerRepeat);
            CHECK(inserted == QString(kKeysPerRepeat, QLatin1Char('a')));
            // Scroll the result from the top, one wheel notch at a time.
            viewport->setProperty("contentY", 0);
            test_support::settleFor("the left pane to follow to the top",
                [&] { return left->property("contentY").toDouble() <= 1; });
            std::vector<qint64> notches;
            std::vector<qint64> notchesWall;
            static quint64 timestamp = 0;
            for (int notch = 0; notch < kNotchesPerRepeat; ++notch) {
                // At the bottom, go back to the top before the next timed notch.
                if (viewport->property("contentY").toDouble() + viewport->height()
                        >= viewport->property("contentHeight").toDouble() - 1) {
                    viewport->setProperty("contentY", 0);
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                }
                const auto from = viewport->property("contentY").toDouble();
                const auto cost = costToFrame(scene.window, [&] {
                    const auto point = viewport->mapToScene(QPointF(viewport->width() / 2, viewport->height() / 2));
                    QWheelEvent wheel(point, scene.window->mapToGlobal(point), QPoint(), QPoint(0, -120), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
                    timestamp += 250;
                    wheel.setTimestamp(timestamp);
                    QCoreApplication::sendEvent(scene.window, &wheel);
                }, [&] { return viewport->property("contentY").toDouble() > from; });
                if (cost.wall < 0) throw std::runtime_error("notch " + std::to_string(notch) + " did not scroll and draw");
                notches.push_back(cost.cpu);
                notchesWall.push_back(cost.wall);
                test_support::settleFor("the scroll to end", [&] { return !viewport->property("moving").toBool(); });
            }
            test_support::settleFor("the left pane to follow",
                [&] { return left->property("contentY").toDouble() > 0; });
            typingMedians.push_back(median(keys));
            scrollMedians.push_back(median(notches));
            std::cout << "repeat " << repeat + 1 << " key CPU ms: " << join(keys) << "; wall ms: " << join(keysWall) << '\n';
            std::cout << "repeat " << repeat + 1 << " notch CPU ms: " << join(notches) << "; wall ms: " << join(notchesWall)
                      << '\n';
        });
    }
    const auto typing = median(typingMedians);
    const auto scroll = median(scrollMedians);
    std::cout << "typing CPU median of repeat medians " << typing << " ms (" << join(typingMedians) << "), limit "
              << kTypingLimitMs << " ms\n";
    std::cout << "scroll CPU median of repeat medians " << scroll << " ms (" << join(scrollMedians) << "), limit "
              << kScrollLimitMs << " ms\n";
    suite.run("typing in the highlighted result stays inside its limit", [&] {
        CHECK(typingMedians.size() == static_cast<std::size_t>(kRepeats));
        CHECK(typing <= kTypingLimitMs);
    });
    suite.run("scrolling the highlighted panes stays inside its limit", [&] {
        CHECK(scrollMedians.size() == static_cast<std::size_t>(kRepeats));
        CHECK(scroll <= kScrollLimitMs);
    });
    suite.run("typing diagnostic replay reaches acknowledged frames", [&] { diagnoseTyping(scene); });
    return suite.report();
#endif
}

// Writes the kdeglobals that makes KColorScheme, and so the KDE style, use scheme.
bool writeColourScheme(const QString &configHome, const QString &scheme)
{
    const auto path = schemeFile(scheme);
    if (path.isEmpty()) return true;
    QFile source(path);
    QFile target(configHome + QStringLiteral("/kdeglobals"));
    if (!QDir().mkpath(configHome) || !source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly))
        return false;
    target.write(source.readAll());
    target.write("\n[General]\nColorScheme=" + scheme.toUtf8() + "\n");
    return true;
}
}

int main(int argc, char **argv)
{
    // Unbuffered, so a run that times out still shows how far it got.
    std::cout << std::unitbuf;
    QTemporaryDir data;
    if (!data.isValid()) return 2;
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QString argument = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    const auto configHome = data.path() + QStringLiteral("/config");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", configHome.toUtf8());
    qputenv("XDG_CACHE_HOME", (data.path() + "/cache").toUtf8());
    if (mode == QStringLiteral("roles") && !writeColourScheme(configHome, argument)) return 2;
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName("BranchTruceThemeTests");
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) return 2;
    if (mode == QStringLiteral("icons") && argc == 3) return checkIcons(argument);
    if (mode == QStringLiteral("roles") && argc == 3) {
        const auto other = argument == QStringLiteral("BreezeDark") ? QStringLiteral("BreezeLight") : QStringLiteral("BreezeDark");
        return checkRoles(argument, other);
    }
    if (mode == QStringLiteral("cues") && argc == 2) return checkCues();
    if (mode == QStringLiteral("cost") && argc == 2) return checkCost();
    std::cerr << "usage: branch_truce_theme_tests icons <icon list> | roles <scheme> | cues | cost\n";
    return 2;
}
