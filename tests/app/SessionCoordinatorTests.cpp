#include "app/ApplicationController.h"
#include "fixtures/GitOnPath.h"
#include "recovery/RecoveryRestore.h"
#include "session/SessionCoordinator.h"
#include "TestHarness.h"
#include "fixtures/ChoiceFile.h"
#include "fixtures/RecoveryJournal.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

using namespace branch_truce;

namespace {
Draft draft(std::string text = "before\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}}, 0};
}

template<typename Predicate>
bool until(Predicate predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return predicate();
}

// Release barriers during unwinding too. Worker lambdas keep shared state,
// never references to a fixture or its model, even when an assertion fails.
struct Gate {
    struct State {
        std::atomic_bool entered{false};
        QSemaphore release;
        void pause()
        {
            entered = true;
            if (!release.tryAcquire(1, 5000)) throw std::runtime_error("worker barrier timed out");
        }
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    ~Gate() { open(); }
    void open() const { state->release.release(); }
    void wait() const { CHECK(until([&] { return state->entered.load(); })); }
};

struct DrainWorkers {
    ~DrainWorkers() { QThreadPool::globalInstance()->waitForDone(6000); }
};

TaskResult writeResult(const TaskInput &input, const TaskControl &control, const QByteArray &bytes)
{
    if (!control.beginMutation()) return {TaskStatus::Cancelled, MutationObservation::NotAttempted, {}, {}, {}};
    const auto path = QFile::encodeName(input.stamp.worktree) + '/' + QByteArray::fromStdString(input.path);
    const int fd = ::open(path.constData(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("fixture mutation open failed");
    const auto written = ::write(fd, bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const int closed = ::close(fd);
    if (written != bytes.size() || closed != 0) throw std::runtime_error("fixture mutation write failed");
    return {TaskStatus::Succeeded, MutationObservation::NotAttempted, bytes, {}, {}};
}

MutationObservation changed(const TaskInput &) { return MutationObservation::ObservedChanged; }

// One line added by the left side, one earlier line removed by the right side
// and one line both sides changed. The result starts as the earlier version.
// The multibyte first line keeps UTF-16 offsets apart from byte offsets.
FileSession viewFile()
{
    const std::string earlier = "héader \U0001F600\nbridge one\nbase removed\nbridge two\nbase conflict\ntail\n";
    auto sources = *test_support::choiceFile(
        "héader \U0001F600\nleft added\nbridge one\nbase removed\nbridge two\nleft conflict\ntail\n",
        "héader \U0001F600\nbridge one\nbridge two\nright conflict\ntail\n").snapshots();
    sources.path = "src/view/choices.cpp";
    sources.earlier = SourceSnapshot{"earlier-object", "100644", earlier,
        test_support::choiceText(earlier), "earlier-commit", "Common earlier version"};
    sources.launchResult.text = test_support::choiceText(earlier);
    return FileSession("choices", sources.path, {earlier, {}, 0}, Support::Supported, {}, sources, true);
}

// The UTF-16 offset where a 1-based line starts, found without the controller.
int lineOffset(const QString &text, int line)
{
    int offset = 0;
    for (int current = 1; current < line; ++current) {
        const auto next = text.indexOf(QLatin1Char('\n'), offset);
        if (next < 0) throw std::runtime_error("fixture text has no such line");
        offset = static_cast<int>(next) + 1;
    }
    return offset;
}

// Line ranges are 1-based and half-open; UTF-16 offsets are editor positions.
void checkRange(const QVariantMap &entry, const QString &prefix, const QString &text, int begin, int end)
{
    for (const auto *key : {"LineBegin", "LineEnd", "Utf16Begin", "Utf16End"})
        CHECK(entry.value(prefix + QLatin1String(key)).typeId() == QMetaType::Int);
    CHECK(entry.value(prefix + "LineBegin").toInt() == begin && entry.value(prefix + "LineEnd").toInt() == end);
    CHECK(entry.value(prefix + "Utf16Begin").toInt() == lineOffset(text, begin));
    CHECK(entry.value(prefix + "Utf16End").toInt() == lineOffset(text, end));
}

struct ExpectedPassage {
    const char *side;
    const char *kind;
    int sourceBegin, sourceEnd, resultBegin, resultEnd;
};

// Every differing passage, in order, after checking that each side's passages
// partition both its source and the result from line 1 to the end.
void checkPassages(const SelectedFile *file, const std::vector<ExpectedPassage> &expected)
{
    const auto result = file->resultText();
    std::vector<QVariantMap> differing;
    for (const auto *side : {"stage2", "stage3", "earlier"}) {
        const auto source = file->property(side).toMap().value("text").toString();
        int sourceLine = 1;
        int resultLine = 1;
        for (const auto &value : file->passages()) {
            const auto passage = value.toMap();
            if (passage.value("side").toString() != QLatin1String(side)) continue;
            CHECK(passage.value("sourceLineBegin").toInt() == sourceLine);
            CHECK(passage.value("resultLineBegin").toInt() == resultLine);
            sourceLine = passage.value("sourceLineEnd").toInt();
            resultLine = passage.value("resultLineEnd").toInt();
            checkRange(passage, "source", source, passage.value("sourceLineBegin").toInt(), sourceLine);
            checkRange(passage, "result", result, passage.value("resultLineBegin").toInt(), resultLine);
            if (passage.value("kind").toString() != "equal") differing.push_back(passage);
        }
        CHECK(sourceLine == static_cast<int>(source.count(QLatin1Char('\n'))) + 1);
        CHECK(resultLine == static_cast<int>(result.count(QLatin1Char('\n'))) + 1);
    }
    CHECK(differing.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(differing[i].value("side").toString() == QLatin1String(expected[i].side));
        CHECK(differing[i].value("kind").toString() == QLatin1String(expected[i].kind));
        CHECK(differing[i].value("sourceLineBegin").toInt() == expected[i].sourceBegin);
        CHECK(differing[i].value("sourceLineEnd").toInt() == expected[i].sourceEnd);
        CHECK(differing[i].value("resultLineBegin").toInt() == expected[i].resultBegin);
        CHECK(differing[i].value("resultLineEnd").toInt() == expected[i].resultEnd);
    }
}

struct ExpectedRegion {
    const char *side;
    const char *scope;
    const char *state;
    int sourceBegin, sourceEnd, resultBegin, resultEnd;
};

// Regions follow the scoped choices one for one, in the same order.
void checkRegions(const SelectedFile *file, const std::vector<ExpectedRegion> &expected)
{
    const auto regions = file->regions();
    const auto choices = file->choices();
    CHECK(regions.size() == static_cast<qsizetype>(expected.size()) && choices.size() == regions.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto region = regions[static_cast<qsizetype>(i)].toMap();
        const auto &want = expected[i];
        CHECK(region.value("id") == choices[static_cast<qsizetype>(i)].toMap().value("id"));
        CHECK(region.value("side").toString() == QLatin1String(want.side));
        CHECK(region.value("scope").toString() == QLatin1String(want.scope));
        CHECK(region.value("state").toString() == QLatin1String(want.state));
        CHECK(region.value("mapped").toBool() && !region.value("section").toString().isEmpty());
        CHECK(region.value("decisionIds").toStringList().size() == 1);
        checkRange(region, "source", file->property(want.side).toMap().value("text").toString(), want.sourceBegin, want.sourceEnd);
        checkRange(region, "result", file->resultText(), want.resultBegin, want.resultEnd);
    }
}

QString regionId(const SelectedFile *file, const char *side, const char *scope, int sourceLine)
{
    for (const auto &value : file->regions()) {
        const auto region = value.toMap();
        if (region.value("side").toString() == QLatin1String(side) && region.value("scope").toString() == QLatin1String(scope)
            && region.value("sourceLineBegin").toInt() == sourceLine) return region.value("id").toString();
    }
    throw std::runtime_error("fixture has no such region");
}

// The hand-edit flag of each region, in region order.
std::vector<bool> handEdits(const SelectedFile *file)
{
    std::vector<bool> result;
    for (const auto &value : file->regions()) {
        const auto flag = value.toMap().value("handEdited");
        CHECK(flag.typeId() == QMetaType::Bool);
        result.push_back(flag.toBool());
    }
    return result;
}

QVariantMap region(const SelectedFile *file, const QString &id)
{
    for (const auto &value : file->regions())
        if (value.toMap().value("id").toString() == id) return value.toMap();
    throw std::runtime_error("fixture has no such region");
}

QStringList regionStates(const SelectedFile *file)
{
    QStringList result;
    for (const auto &value : file->regions()) result.push_back(value.toMap().value("state").toString());
    return result;
}

// viewFile() regions 6 to 9 cover the line both sides changed.
const std::vector<bool> noHandEdits(10, false);
const std::vector<bool> conflictLineEdited{false, false, false, false, false, false, true, true, true, true};

// One file in a controller, edited through its model and read back as the view sees it.
struct RegionView {
    explicit RegionView(FileSession file = viewFile())
    {
        CHECK(controller.files()->appendFile(std::move(file)));
        CHECK(controller.selectFile("choices"));
    }
    RevisionId revision() { return controller.files()->file("choices")->current().id; }
    const std::string &text() { return controller.files()->file("choices")->current().draft.text; }
    SelectedFile *file() { return controller.selectedFile(); }
    SelectedFile *edit(ResultEdit edit)
    {
        CHECK(controller.files()->editResult("choices", revision(), std::move(edit)) == Transition::Applied);
        return file();
    }
    // Types at an offset into the first occurrence of some result text.
    SelectedFile *type(const std::string &near, std::size_t offset, std::string typed)
    {
        const auto at = text().find(near);
        CHECK(at != std::string::npos);
        return edit({{at + offset, at + offset}, std::move(typed)});
    }
    SelectedFile *choose(const QString &id, ChoiceAction action)
    {
        const auto preview = previewChoice(*controller.files()->file("choices"), revision(), id.toStdString(), action);
        CHECK(controller.files()->applyChoice("choices", preview, true) == Transition::Applied);
        return file();
    }
    SelectedFile *undo()
    {
        CHECK(controller.files()->undo("choices", revision()) == Transition::Applied);
        return file();
    }
    SelectedFile *redo()
    {
        CHECK(controller.files()->redo("choices", revision()) == Transition::Applied);
        return file();
    }
    ApplicationController controller{gitOnPath()};
};

class Fixture {
public:
    Fixture()
    {
        CHECK(root.isValid());
        git({"init", "--quiet", "--initial-branch=main"});
        write("tracked.txt", "before\n");
        git({"add", "--", "tracked.txt"});
        coordinator = std::make_unique<SessionCoordinator>(model, "/usr/bin/git");
        QObject::connect(coordinator.get(), &SessionCoordinator::taskFinished, &model,
                         [this](const TaskCompletion &value) { finished.push_back(value); });
        replace();
    }
    ~Fixture()
    {
        coordinator.reset();
        for (const auto &path : rawFiles) ::unlink(path.constData());
    }
    void replace(const QByteArray &path = "tracked.txt", const QString &operation = "merge")
    {
        CHECK(coordinator->replaceSession(root.path(), operation,
              {FileSession("file", path.toStdString(), draft())}));
    }
    void write(const QByteArray &relative, const QByteArray &bytes)
    {
        const auto path = QFile::encodeName(root.path()) + '/' + relative;
        const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(fd >= 0);
        rawFiles.push_back(path);
        const auto written = ::write(fd, bytes.constData(), static_cast<std::size_t>(bytes.size()));
        const int closed = ::close(fd);
        CHECK(written == bytes.size() && closed == 0);
    }
    QByteArray git(const QStringList &arguments, const QByteArray &input = {}) const
    {
        QProcess process;
        QProcessEnvironment environment;
        environment.insert("PATH", "/usr/bin:/bin");
        environment.insert("LC_ALL", "C");
        environment.insert("GIT_CONFIG_NOSYSTEM", "1");
        environment.insert("GIT_CONFIG_GLOBAL", "/dev/null");
        environment.insert("GIT_TERMINAL_PROMPT", "0");
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root.path());
        auto argv = QStringList{"--no-pager", "--literal-pathspecs"};
        argv.append(arguments);
        process.start("/usr/bin/git", argv);
        CHECK(process.waitForStarted(5000));
        CHECK(process.write(input) == input.size());
        process.closeWriteChannel();
        CHECK(process.waitForFinished(5000));
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            throw std::runtime_error("fixture git: " + process.readAllStandardError().toStdString());
        return process.readAllStandardOutput();
    }
    void wait(std::size_t count)
    {
        CHECK(until([&] { return finished.size() == count && coordinator->pendingTasks() == 0; }));
        QCoreApplication::processEvents();
        CHECK(finished.size() == count);
    }
    QTemporaryDir root{QStringLiteral("/tmp/truce-coordinator-XXXXXX")};
    SessionModel model;
    std::unique_ptr<SessionCoordinator> coordinator;
    std::vector<TaskCompletion> finished;
    QList<QByteArray> rawFiles;
};
}

class SessionCoordinatorTests {
public:
    int run()
    {
        test_support::Suite suite("SessionCoordinatorTests");
        const auto test = [&](const char *name, auto body) {
            suite.run(name, [&] { DrainWorkers drain; body(); });
        };
        test("the selected file exposes passage kinds, region ranges, decisions and the file name", [] {
            ApplicationController controller(gitOnPath());
            CHECK(controller.files()->appendFile(viewFile()));
            CHECK(controller.selectFile("choices"));
            const auto file = controller.selectedFile();
            CHECK(file->fileName() == "choices.cpp" && file->path() == "src/view/choices.cpp");
            CHECK(file->pathBytes() == "src/view/choices.cpp" && file->currentRevision() == "1");
            CHECK(file->decisions() == controller.files()->data(controller.files()->index(0), SessionModel::DecisionsRole).toList());
            CHECK(file->decisions().size() == 6);
            checkPassages(file, {{"stage2", "source-only", 2, 3, 2, 2}, {"stage2", "changed", 6, 7, 5, 6},
                {"stage3", "result-only", 3, 3, 3, 4}, {"stage3", "changed", 4, 5, 5, 6}});
            checkRegions(file, {{"stage2", "section", "undecided", 2, 3, 2, 2}, {"stage2", "line", "undecided", 2, 3, 2, 2},
                {"stage3", "section", "undecided", 2, 2, 2, 2},
                {"stage2", "section", "undecided", 4, 5, 3, 4}, {"stage2", "line", "undecided", 4, 5, 3, 4},
                {"stage3", "section", "undecided", 3, 3, 4, 4},
                {"stage2", "section", "undecided", 6, 7, 5, 6}, {"stage2", "line", "undecided", 6, 7, 5, 6},
                {"stage3", "section", "undecided", 4, 5, 5, 6}, {"stage3", "line", "undecided", 4, 5, 5, 6}});
            const auto choices = file->choices();
            CHECK(choices[6].toMap().value("label").toString() == QString::fromUtf8("Left section, lines 6–6"));
            CHECK(choices[6].toMap().value("state").toString() == "undecided");
        });
        test("selected file notifies changed parts without re-emitting sources on edits", [] {
            ApplicationController controller(gitOnPath());
            CHECK(controller.files()->appendFile(viewFile()));
            CHECK(controller.selectFile("choices"));
            auto *selected = controller.selectedFile();
            QSignalSpy identity(selected, &SelectedFile::identityChanged);
            QSignalSpy sources(selected, &SelectedFile::sourcesChanged);
            QSignalSpy result(selected, &SelectedFile::resultTextChanged);
            QSignalSpy decisions(selected, &SelectedFile::decisionsChanged);
            CHECK(identity.isValid() && sources.isValid() && result.isValid() && decisions.isValid());
            const auto source = selected->stage2();
            const auto revision = [&] { return controller.files()->file("choices")->current().id; };
            CHECK(controller.files()->editResult("choices", revision(), {{0, 0}, "typed "}) == Transition::Applied);
            CHECK(controller.selectedFile() == selected);
            CHECK(identity.count() == 0 && sources.count() == 0 && result.count() == 1 && decisions.count() == 0);
            CHECK(selected->stage2() == source && selected->resultText().startsWith("typed "));
            CHECK(selected->currentRevision()
                == controller.files()->data(controller.files()->index(0), SessionModel::CurrentRevisionRole).toString());
            const auto target = regionId(selected, "stage2", "line", 6);
            const auto preview = previewChoice(*controller.files()->file("choices"), revision(),
                                               target.toStdString(), ChoiceAction::Ignore);
            CHECK(controller.files()->applyChoice("choices", preview) == Transition::Applied);
            CHECK(identity.count() == 0 && sources.count() == 0 && result.count() == 1 && decisions.count() == 1);
        });
        test("accepted controller edit keeps source signals and row status in sync", [] {
            ApplicationController controller(gitOnPath());
            CHECK(controller.files()->appendFile(viewFile()));
            CHECK(controller.selectFile("choices"));
            auto *selected = controller.selectedFile();
            const auto row = controller.files()->index(0);
            const auto originalSource = selected->stage2();
            const auto originalText = selected->resultText();
            const auto originalRevision = selected->currentRevision();
            QSignalSpy sources(selected, &SelectedFile::sourcesChanged);
            QSignalSpy identity(selected, &SelectedFile::identityChanged);
            QSignalSpy result(selected, &SelectedFile::resultTextChanged);
            QSignalSpy status(selected, &SelectedFile::statusChanged);
            QSignalSpy rows(controller.files(), &QAbstractItemModel::dataChanged);
            CHECK(sources.isValid() && identity.isValid() && result.isValid()
                  && status.isValid() && rows.isValid());
            CHECK(controller.editResult("choices", originalRevision, 0, 0, QString::fromUtf8("é ")));
            CHECK(controller.selectedFile() == selected && selected->stage2() == originalSource);
            CHECK(sources.count() == 0 && identity.count() == 0 && result.count() == 1);
            CHECK(status.count() == 1 && rows.count() == 1);
            CHECK(selected->resultText() == QString::fromUtf8("é ") + originalText);
            CHECK(controller.files()->file("choices")->current().draft.text
                  == selected->resultText().toUtf8().toStdString());
            CHECK(selected->currentRevision() == row.data(SessionModel::CurrentRevisionRole).toString());
            CHECK(selected->status() == row.data(SessionModel::StatusRole).toString());
            CHECK(selected->progress() == row.data(SessionModel::ProgressRole).toMap());
            CHECK(!controller.editResult("choices", originalRevision, 0, 0, "stale"));
            CHECK(sources.count() == 0 && identity.count() == 0 && result.count() == 1
                  && status.count() == 1 && rows.count() == 1);
        });
        test("selected file ranges, kinds and region states follow each new revision", [] {
            ApplicationController controller(gitOnPath());
            CHECK(controller.files()->appendFile(viewFile()));
            CHECK(controller.selectFile("choices"));
            int changes = 0;
            QObject::connect(&controller, &ApplicationController::selectedFileChanged, [&] { ++changes; });
            const auto revision = [&] { return controller.files()->file("choices")->current().id; };

            CHECK(controller.files()->editResult("choices", revision(), {{0, 0}, "inserted é\U0001F600\n"}) == Transition::Applied);
            auto file = controller.selectedFile();
            CHECK(changes > 0 && file->currentRevision() == "2");
            checkPassages(file, {{"stage2", "result-only", 1, 1, 1, 2}, {"stage2", "source-only", 2, 3, 3, 3},
                {"stage2", "changed", 6, 7, 6, 7}, {"stage3", "result-only", 1, 1, 1, 2},
                {"stage3", "result-only", 3, 3, 4, 5}, {"stage3", "changed", 4, 5, 6, 7},
                {"earlier", "result-only", 1, 1, 1, 2}});
            checkRegions(file, {{"stage2", "section", "undecided", 2, 3, 3, 3}, {"stage2", "line", "undecided", 2, 3, 3, 3},
                {"stage3", "section", "undecided", 2, 2, 3, 3},
                {"stage2", "section", "undecided", 4, 5, 4, 5}, {"stage2", "line", "undecided", 4, 5, 4, 5},
                {"stage3", "section", "undecided", 3, 3, 5, 5},
                {"stage2", "section", "undecided", 6, 7, 6, 7}, {"stage2", "line", "undecided", 6, 7, 6, 7},
                {"stage3", "section", "undecided", 4, 5, 6, 7}, {"stage3", "line", "undecided", 4, 5, 6, 7}});

            const auto choose = [&](const QString &id, ChoiceAction action) {
                const auto preview = previewChoice(*controller.files()->file("choices"), revision(), id.toStdString(), action);
                CHECK(controller.files()->applyChoice("choices", preview, true) == Transition::Applied);
            };
            changes = 0;
            choose(regionId(file, "stage2", "line", 6), ChoiceAction::Accept);
            choose(regionId(file, "stage2", "line", 2), ChoiceAction::Ignore);
            file = controller.selectedFile();
            CHECK(changes > 0 && file->currentRevision() == "4");
            CHECK(file->decisions() == controller.files()->data(controller.files()->index(0), SessionModel::DecisionsRole).toList());
            // The accepted left line now equals the result, so only the right side still differs there.
            checkPassages(file, {{"stage2", "result-only", 1, 1, 1, 2}, {"stage2", "source-only", 2, 3, 3, 3},
                {"stage3", "result-only", 1, 1, 1, 2}, {"stage3", "result-only", 3, 3, 4, 5},
                {"stage3", "changed", 4, 5, 6, 7}, {"earlier", "result-only", 1, 1, 1, 2},
                {"earlier", "changed", 5, 6, 6, 7}});
            checkRegions(file, {{"stage2", "section", "ignored", 2, 3, 3, 3}, {"stage2", "line", "ignored", 2, 3, 3, 3},
                {"stage3", "section", "undecided", 2, 2, 3, 3},
                {"stage2", "section", "undecided", 4, 5, 4, 5}, {"stage2", "line", "undecided", 4, 5, 4, 5},
                {"stage3", "section", "undecided", 3, 3, 5, 5},
                {"stage2", "section", "accepted", 6, 7, 6, 7}, {"stage2", "line", "accepted", 6, 7, 6, 7},
                {"stage3", "section", "undecided", 4, 5, 6, 7}, {"stage3", "line", "undecided", 4, 5, 6, 7}});

            // A hand edit inside accepted text marks it manual and leaves the undecided competitor alone.
            const auto at = controller.files()->file("choices")->current().draft.text.find("left conflict") + 5;
            CHECK(controller.files()->editResult("choices", revision(), {{at, at}, "hand "}) == Transition::Applied);
            file = controller.selectedFile();
            CHECK(file->currentRevision() == "5");
            const auto regions = file->regions();
            CHECK(regions[6].toMap().value("state").toString() == "manual" && regions[7].toMap().value("state").toString() == "manual");
            CHECK(regions[8].toMap().value("state").toString() == "undecided" && regions[9].toMap().value("state").toString() == "undecided");
            checkRange(regions[6].toMap(), "result", file->resultText(), 6, 7);
            CHECK(controller.files()->undo("choices", revision()) == Transition::Applied);
            file = controller.selectedFile();
            CHECK(file->regions()[6].toMap().value("state").toString() == "accepted");
            CHECK(file->resultText().contains("\nleft conflict\n"));
        });
        test("no region reports a hand edit at launch, even where the result already differs", [] {
            // The earlier version, the left side, and conflict markers around both sides.
            const auto marked = test_support::choiceFile(
                "header\nleft first\nleft second\nbridge\nleft later\ntail\n",
                "header\nright first\nright second\nbridge\nright later\ntail\n",
                "header\n<<<<<<< ours\nleft first\nleft second\n=======\nright first\nright second\n>>>>>>> theirs\n"
                "bridge\n<<<<<<< ours\nleft later\n=======\nright later\n>>>>>>> theirs\ntail\n");
            std::size_t overlapping = 0;
            for (const auto &session : {viewFile(), test_support::choiceFile(), marked}) {
                RegionView view(session);
                const auto file = view.file();
                const auto edits = handEdits(file);
                CHECK(!edits.empty() && std::none_of(edits.begin(), edits.end(), [](bool edited) { return edited; }));
                CHECK(!regionStates(file).contains("manual"));
                // Replacing launch text that differs from its source still needs confirmation.
                for (const auto &value : file->regions()) {
                    const auto *anchor = view.controller.files()->file("choices")->correspondence()
                        ->anchor(value.toMap().value("id").toString().toStdString());
                    if (anchor && anchor->manualOverlap) ++overlapping;
                }
            }
            CHECK(overlapping > 0);
        });
        test("TRUCE-0177 deciding both sides of a marked section removes its marker lines from the shown result", [] {
            RegionView view(test_support::markedFile());
            auto file = view.file();
            const auto preview = [&](const char *side, ChoicePreviewFlow::Action action) {
                return view.controller.previewChoice("choices", file->currentRevision(),
                                                     regionId(file, side, "section", 2), action);
            };
            CHECK(preview("stage2", ChoicePreviewFlow::Action::Accept));
            CHECK(view.controller.commandMessage() == "Accept changes only this scope and leaves the other side undecided.");
            CHECK(view.controller.applyChoice());
            file = view.file();
            CHECK(file->resultText() == QString::fromStdString(test_support::markedText()));
            CHECK(preview("stage3", ChoicePreviewFlow::Action::Ignore));
            CHECK(view.controller.commandMessage().contains("marker lines and ignored text are removed"));
            CHECK(view.controller.applyChoice());
            file = view.file();
            const auto resolved = QStringLiteral("header\nleft first\nleft second\nbridge\n"
                "<<<<<<< ours\nleft later\n=======\nright later\n>>>>>>> theirs\ntail\n");
            CHECK(file->resultText() == resolved);
            CHECK(region(file, regionId(file, "stage2", "section", 2)).value("state").toString() == "accepted");
            CHECK(view.controller.files()->file("choices")->current().draft.unresolvedMarkers == 1);
            CHECK(view.undo()->resultText() == QString::fromStdString(test_support::markedText()));
            CHECK(view.redo()->resultText() == resolved);
        });
        test("a hand edit marks the regions containing it in every decision state until a choice replaces it", [] {
            RegionView view;
            auto file = view.file();
            const auto leftConflict = regionId(file, "stage2", "line", 6);
            const auto rightConflict = regionId(file, "stage3", "line", 4);
            const auto removed = regionId(file, "stage2", "line", 4);
            const auto deletion = regionId(file, "stage3", "section", 3);
            auto expected = QStringList(10, "undecided");

            // Undecided regions report the edit and stay undecided.
            file = view.type("base conflict", 5, "hand ");
            CHECK(handEdits(file) == conflictLineEdited && regionStates(file) == expected);
            file = view.choose(leftConflict, ChoiceAction::Accept);
            expected[6] = expected[7] = "accepted";
            CHECK(handEdits(file) == noHandEdits && regionStates(file) == expected);

            // Accepted.
            file = view.type("left conflict", 5, "hand ");
            expected[6] = expected[7] = "manual";
            CHECK(handEdits(file) == conflictLineEdited && regionStates(file) == expected);
            file = view.choose(rightConflict, ChoiceAction::CombineLeftFirst);
            for (const int i : {6, 7, 8, 9}) expected[i] = "accepted";
            CHECK(handEdits(file) == noHandEdits && regionStates(file) == expected);
            CHECK(file->resultText().contains("\nleft conflict\nright conflict\n"));

            // Combined.
            file = view.type("right conflict", 6, "hand ");
            for (const int i : {6, 7, 8, 9}) expected[i] = "manual";
            CHECK(handEdits(file) == conflictLineEdited && regionStates(file) == expected);
            file = view.choose(leftConflict, ChoiceAction::CombineRightFirst);
            for (const int i : {6, 7, 8, 9}) expected[i] = "accepted";
            CHECK(handEdits(file) == noHandEdits && regionStates(file) == expected);
            CHECK(file->resultText().contains("\nright conflict\nleft conflict\n"));

            // Ignored. Ignoring keeps the text, so only accepting the deletion replaces the edit.
            file = view.choose(removed, ChoiceAction::Ignore);
            expected[3] = expected[4] = "ignored";
            CHECK(handEdits(file) == noHandEdits && regionStates(file) == expected);
            file = view.type("base removed", 5, "hand ");
            expected[3] = expected[4] = "manual";
            CHECK(handEdits(file) == std::vector<bool>{false, false, false, true, true, false, false, false, false, false});
            CHECK(regionStates(file) == expected);
            file = view.choose(deletion, ChoiceAction::Accept);
            expected[3] = expected[4] = "ignored";
            expected[5] = "accepted";
            CHECK(handEdits(file) == noHandEdits && regionStates(file) == expected);
            CHECK(!file->resultText().contains("removed"));
        });
        test("typing across a region boundary marks it and a whole-file choice clears every hand edit", [] {
            RegionView crossing;
            auto file = crossing.choose(regionId(crossing.file(), "stage2", "line", 6), ChoiceAction::Accept);
            const auto at = crossing.text().find("two\nleft conflict");
            CHECK(at != std::string::npos);
            file = crossing.edit({{at, at + 8}, "2\nright"});
            const auto regions = file->regions();
            CHECK(handEdits(file) == conflictLineEdited && !regions[6].toMap().value("mapped").toBool());
            CHECK(regionStates(file)[6] == "manual" && regionStates(file)[8] == "undecided");

            RegionView view;
            view.type("base conflict", 5, "hand ");
            file = view.type("base removed", 5, "hand ");
            CHECK(handEdits(file) == std::vector<bool>{false, false, false, true, true, false, true, true, true, true});
            auto &files = *view.controller.files();
            const auto preview = previewWholeFile(*files.file("choices"), view.revision(), SourceSide::Stage2);
            CHECK(files.applyWholeFile("choices", preview, true) == Transition::Applied);
            file = view.file();
            CHECK(handEdits(file) == noHandEdits);
            CHECK(!regionStates(file).contains("manual") && !regionStates(file).contains("undecided"));
        });
        test("undo and redo restore each region's hand-edit flag with the text and decisions", [] {
            RegionView view;
            const auto leftConflict = regionId(view.file(), "stage2", "line", 6);
            auto file = view.type("base conflict", 5, "hand ");
            CHECK(handEdits(file) == conflictLineEdited);
            file = view.choose(leftConflict, ChoiceAction::Accept);
            CHECK(handEdits(file) == noHandEdits && regionStates(file)[6] == "accepted");
            file = view.undo();
            CHECK(handEdits(file) == conflictLineEdited && regionStates(file)[6] == "undecided");
            CHECK(file->resultText().contains("\nbase hand conflict\n"));
            file = view.undo();
            CHECK(handEdits(file) == noHandEdits && file->resultText().contains("\nbase conflict\n"));
            file = view.redo();
            CHECK(handEdits(file) == conflictLineEdited && file->resultText().contains("\nbase hand conflict\n"));
            file = view.redo();
            CHECK(handEdits(file) == noHandEdits && regionStates(file)[6] == "accepted");
            CHECK(file->resultText().contains("\nleft conflict\n"));
        });
        test("a choice clears hand edits on a deleted line it restores and on text an edit left unchanged", [] {
            const auto none = [](const SelectedFile *file) {
                const auto edits = handEdits(file);
                return std::none_of(edits.begin(), edits.end(), [](bool edited) { return edited; });
            };
            const auto left = test_support::choiceFile().snapshots()->stage2->worktree.utf8;

            // Deleting a line by hand invalidates it, so only its source places it in the section.
            RegionView deleted(test_support::choiceFile());
            auto file = deleted.file();
            const auto line = regionId(file, "stage2", "line", 2);
            const auto at = deleted.text().find("left first\n");
            file = deleted.edit({{at, at + 11}, ""});
            CHECK(region(file, line).value("handEdited").toBool() && !region(file, line).value("mapped").toBool());
            file = deleted.choose(regionId(file, "stage2", "section", 2), ChoiceAction::Accept);
            CHECK(deleted.text() == left && none(file) && region(file, line).value("state") == "accepted");
            file = deleted.undo();
            CHECK(region(file, line).value("handEdited").toBool() && region(file, line).value("state") == "undecided");
            file = deleted.redo();
            CHECK(none(file) && region(file, line).value("state") == "accepted");

            RegionView unchanged(test_support::choiceFile());
            auto &files = *unchanged.controller.files();
            const auto wholeLeft = [&] {
                return files.applyWholeFile("choices",
                    previewWholeFile(*files.file("choices"), unchanged.revision(), SourceSide::Stage2), true);
            };
            CHECK(wholeLeft() == Transition::Applied);
            const auto typed = unchanged.text().find("left first") + 5;
            unchanged.edit({{typed, typed}, "X"});
            file = unchanged.edit({{typed, typed + 1}, ""});
            const auto manual = handEdits(file);
            CHECK(unchanged.text() == left && !none(file) && regionStates(file).contains("manual"));
            // The same choice leaves text and decisions as they are but replaces the edit.
            CHECK(wholeLeft() == Transition::Applied);
            file = unchanged.file();
            CHECK(unchanged.text() == left && none(file) && !regionStates(file).contains("manual"));
            CHECK(wholeLeft() == Transition::NoChange);
            file = unchanged.undo();
            CHECK(handEdits(file) == manual && regionStates(file).contains("manual"));
            file = unchanged.redo();
            CHECK(none(file) && !regionStates(file).contains("manual"));
        });
        test("recovery replay restores hand edits cleared over a deleted line or unchanged text", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionModel model(directory.path() + "/recovery");
            CHECK(model.appendFile(test_support::choiceFile()));
            const auto revision = [&] { return model.file("choices")->current().id; };
            const auto edit = [&](ResultEdit value) {
                CHECK(model.editResult("choices", revision(), std::move(value)) == Transition::Applied);
            };
            const auto replayed = [&] {
                const auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
                CHECK(replay.succeeded && replay.files.size() == 1);
                CHECK(replay.files.front().current().draft == model.file("choices")->current().draft);
                RegionView live(*model.file("choices"));
                RegionView restored(replay.files.front());
                CHECK(handEdits(restored.file()) == handEdits(live.file()));
                CHECK(regionStates(restored.file()) == regionStates(live.file()));
                return handEdits(restored.file());
            };
            const auto cleared = std::vector<bool>(handEdits(RegionView(test_support::choiceFile()).file()).size(), false);

            const auto at = model.file("choices")->current().draft.text.find("left first\n");
            edit({{at, at + 11}, ""});
            const auto deleted = replayed();
            const auto section = test_support::choiceTarget(
                *model.file("choices"), SourceSide::Stage2, ChoiceScope::Section).source.contributionId;
            CHECK(model.applyChoice("choices", previewChoice(*model.file("choices"), revision(), section,
                ChoiceAction::Accept), true) == Transition::Applied);
            CHECK(replayed() == cleared);
            CHECK(model.undo("choices", revision()) == Transition::Applied);
            CHECK(replayed() == deleted);
            CHECK(model.redo("choices", revision()) == Transition::Applied);
            CHECK(replayed() == cleared);

            const auto wholeLeft = [&] {
                return model.applyWholeFile("choices",
                    previewWholeFile(*model.file("choices"), revision(), SourceSide::Stage2), true);
            };
            CHECK(wholeLeft() == Transition::Applied);
            const auto typed = model.file("choices")->current().draft.text.find("left first") + 5;
            edit({{typed, typed}, "X"});
            edit({{typed, typed + 1}, ""});
            const auto manual = replayed();
            CHECK(manual != cleared);
            // This record repeats the previous draft and differs only in its hand edits.
            CHECK(wholeLeft() == Transition::Applied);
            CHECK(replayed() == cleared);
            CHECK(model.undo("choices", revision()) == Transition::Applied);
            CHECK(replayed() == manual);
            CHECK(model.redo("choices", revision()) == Transition::Applied);
            CHECK(replayed() == cleared);
        });
        test("recovery replay restores each region's hand-edit flag with the text and decisions", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionModel model(directory.path() + "/recovery");
            CHECK(model.appendFile(viewFile()));
            const auto revision = [&] { return model.file("choices")->current().id; };
            const auto type = [&](const std::string &near, const std::string &typed) {
                const auto at = model.file("choices")->current().draft.text.find(near) + 5;
                CHECK(model.editResult("choices", revision(), {{at, at}, typed}) == Transition::Applied);
            };
            // A fresh replay of the journal shows the same regions as the live model.
            const auto replayed = [&] {
                const auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
                CHECK(replay.succeeded && replay.files.size() == 1);
                CHECK(replay.files.front().current().draft == model.file("choices")->current().draft);
                RegionView live(*model.file("choices"));
                RegionView restored(replay.files.front());
                const auto file = restored.file();
                CHECK(handEdits(file) == handEdits(live.file()) && regionStates(file) == regionStates(live.file()));
                return std::make_pair(handEdits(file), regionStates(file));
            };
            type("base conflict", "hand ");
            CHECK(replayed().first == conflictLineEdited);
            const auto leftConflict = test_support::choiceTarget(
                *model.file("choices"), SourceSide::Stage2, ChoiceScope::Line, 2).source.contributionId;
            CHECK(model.applyChoice("choices", previewChoice(*model.file("choices"), revision(), leftConflict,
                ChoiceAction::Accept), true) == Transition::Applied);
            // Replay applies the choice as a whole-draft replacement, which must not count as typing.
            auto snapshot = replayed();
            CHECK(snapshot.first == noHandEdits && snapshot.second[6] == "accepted");
            type("left conflict", "hand ");
            type("base removed", "hand ");
            CHECK(model.undo("choices", revision()) == Transition::Applied);
            snapshot = replayed();
            CHECK(snapshot.first == conflictLineEdited && snapshot.second[6] == "manual");
            CHECK(model.redo("choices", revision()) == Transition::Applied);
            snapshot = replayed();
            CHECK(snapshot.first == std::vector<bool>{false, false, false, true, true, false, true, true, true, true});
        });
        test("journals without recorded hand edits replay with none and unknown anchors are rejected", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionModel model(directory.path() + "/recovery");
            CHECK(model.appendFile(viewFile()));
            const auto at = model.file("choices")->current().draft.text.find("base conflict") + 5;
            CHECK(model.editResult("choices", 1, {{at, at}, "hand "}) == Transition::Applied);
            const auto recorded = model.file("choices")->correspondence()->handEditedAnchors();
            CHECK(recorded.size() == 4);
            // A writer from before the field existed recorded the same draft without
            // it, or the hand-edited ranges recorded after it.
            test_support::rewriteLastRecoveryState(model.recoveryDirectory(), [&](QJsonObject &state) {
                auto current = state.value("current").toObject();
                CHECK(current.value("handEdited").toArray().size() == static_cast<qsizetype>(recorded.size()));
                CHECK(current.value("handEditedRanges").toArray().size() == 1);
                current.remove("handEdited");
                current.remove("handEditedRanges");
                state.insert("current", current);
            });
            auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.size() == 1);
            CHECK(replay.files.front().current().draft == model.file("choices")->current().draft);
            CHECK(replay.files.front().correspondence()->handEditedAnchors().empty());
            CHECK(replay.files.front().correspondence()->handEditedRanges().empty());
            // Every recorded hand edit must name an anchor of the replayed draft.
            test_support::rewriteLastRecoveryState(model.recoveryDirectory(), [](QJsonObject &state) {
                auto current = state.value("current").toObject();
                current.insert("handEdited", QJsonArray{state.value("path")});
                state.insert("current", current);
            });
            replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
            CHECK(!replay.succeeded && replay.files.empty());
        });
        test("background snapshots leave the event loop responsive and complete on its thread", [] {
            Fixture f;
            Gate gate;
            auto *guiThread = QThread::currentThread();
            auto workerThread = std::make_shared<std::atomic_bool>(false);
            int applied = 0;
            bool timerFired = false;
            const auto id = f.coordinator->submit("file", 1,
                [state = gate.state, workerThread, guiThread](const TaskInput &input, const TaskControl &control) {
                    *workerThread = QThread::currentThread() != guiThread && !control.beginMutation();
                    state->pause();
                    return TaskResult{TaskStatus::Succeeded, MutationObservation::NotAttempted,
                                      QByteArray::fromStdString(input.revision.draft.text), {}, {}};
                }, [&](const TaskCompletion &completion) {
                    CHECK(QThread::currentThread() == guiThread);
                    CHECK(completion.disposition == TaskDisposition::Current);
                    ++applied;
                });
            CHECK(id != 0 && f.coordinator->pendingTasks() == 1 && f.finished.empty() && applied == 0);
            gate.wait();
            QTimer::singleShot(0, &f.model, [&] { timerFired = true; });
            CHECK(until([&] { return timerFired; }));
            CHECK(workerThread->load() && f.finished.empty() && f.coordinator->pendingTasks() == 1);
            gate.open();
            f.wait(1);
            CHECK(applied == 1 && f.finished[0].result.data == "before\n");
            const auto &stamp = f.finished[0].stamp;
            CHECK(stamp.id == id && stamp.generation == f.coordinator->generation());
            CHECK(stamp.worktree == f.root.path() && stamp.operation == "merge" && stamp.fileId == "file");
            CHECK(stamp.inputRevision == 1 && stamp.resultRevision == 1 && stamp.decisionRevision == 1);
            CHECK(f.coordinator->mutationOutcomes().empty());
        });
        test("invalid requests never dispatch or emit a completion", [] {
            Fixture f;
            const auto work = [](const TaskInput &, const TaskControl &) { return TaskResult{}; };
            CHECK(f.coordinator->submit("missing", 1, work) == 0);
            CHECK(f.coordinator->submit("file", 2, work) == 0);
            CHECK(f.coordinator->submit("file", 1, {}) == 0);
            CHECK(f.coordinator->submitMutation("file", 1, work, {}) == 0);
            CHECK(!f.coordinator->cancel(999));
            CHECK(!f.coordinator->replaceSession(f.root.path(), "merge",
                  {FileSession("same", "one", draft()), FileSession("same", "two", draft())}));
            CHECK(!f.coordinator->replaceSession(f.root.path() + "/absent", "merge", {}));
            QCoreApplication::processEvents();
            CHECK(f.coordinator->generation() == 1 && f.coordinator->pendingTasks() == 0 && f.finished.empty());
        });
        test("result and decision edits fence late worker snapshots", [] {
            for (const bool decisionsOnly : {false, true}) {
                Fixture f;
                Gate gate;
                int applied = 0;
                CHECK(f.coordinator->submit("file", 1,
                    [state = gate.state](const TaskInput &input, const TaskControl &) {
                        state->pause();
                        return TaskResult{TaskStatus::Succeeded, MutationObservation::NotAttempted,
                                          QByteArray::fromStdString(input.revision.draft.text), {}, {}};
                    }, [&](const TaskCompletion &) { ++applied; }) != 0);
                gate.wait();
                auto edited = draft(decisionsOnly ? "before\n" : "edited\n");
                if (decisionsOnly) edited.decisions["left"] = {DecisionValue::Ignored, {}};
                CHECK(f.model.replaceDraft("file", 1, edited) == Transition::Applied);
                gate.open();
                f.wait(1);
                CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleRevision);
                CHECK(f.finished[0].result.data == "before\n" && f.model.file("file")->current().draft == edited);
            }
        });
        test("editing during mutation inspection retains evidence without applying it to the new revision", [] {
            Fixture f;
            Gate inspect;
            int applied = 0;
            const auto id = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "old result\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; },
                [&](const TaskCompletion &) { ++applied; });
            CHECK(id != 0);
            inspect.wait();
            CHECK(f.model.replaceDraft("file", 1, draft("new draft\n")) == Transition::Applied);
            inspect.open();
            f.wait(1);
            CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleRevision);
            CHECK(f.finished[0].result.status == TaskStatus::Succeeded);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged);
            CHECK(f.finished[0].stamp.inputRevision == 1 && f.finished[0].result.data == "old result\n");
            CHECK(f.model.file("file")->current().draft.text == "new draft\n");
            CHECK(f.coordinator->mutationOutcomes().size() == 1 && f.coordinator->mutationOutcomes()[0].stamp.id == id);
        });
        test("session replacement fences work even with the same file identity", [] {
            Fixture f;
            Gate gate;
            int applied = 0;
            CHECK(f.coordinator->submit("file", 1,
                [state = gate.state](const TaskInput &, const TaskControl &) {
                    state->pause();
                    return TaskResult{};
                }, [&](const TaskCompletion &) { ++applied; }) != 0);
            gate.wait();
            f.replace("tracked.txt", "rebase");
            CHECK(f.coordinator->generation() == 2 && f.coordinator->pendingTasks() == 1);
            gate.open();
            f.wait(1);
            CHECK(applied == 0 && f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[0].stamp.generation == 1 && f.finished[0].stamp.operation == "merge");
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled);
        });
        test("worker inspection and Git staging share a FIFO mutation queue", [] {
            Fixture f;
            Gate inspect;
            const auto original = f.git({"ls-files", "--stage", "-z"});
            const auto expected = f.git({"hash-object", "--stdin"}, "first\n").trimmed();
            const auto first = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "first\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; });
            const auto second = f.coordinator->submitGit("file", 1, {git::Operation::StageFile, {}, "tracked.txt", {}, 0, 0});
            const auto third = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "third\n"); }, changed);
            CHECK(first != 0 && second > first && third > second);
            inspect.wait();
            CHECK(f.coordinator->submit("file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; }) != 0);
            CHECK(until([&] { return f.finished.size() == 1; }));
            CHECK(f.finished[0].result.mutation == MutationObservation::NotAttempted);
            CHECK(f.git({"ls-files", "--stage", "-z"}) == original);
            inspect.open();
            f.wait(4);
            CHECK(f.finished[1].stamp.id == first && f.finished[2].stamp.id == second && f.finished[3].stamp.id == third);
            CHECK(f.finished[2].result.gitResult.has_value());
            CHECK(f.finished[2].result.gitResult->outcome == git::Outcome::Success);
            CHECK(f.finished[2].result.gitResult->indexAfter == "100644 " + expected + " 0\ttracked.txt" + QByteArray(1, '\0'));
            CHECK(f.git({"show", ":tracked.txt"}) == "first\n");
            QFile file(f.root.path() + "/tracked.txt");
            CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "third\n");
            CHECK(f.coordinator->mutationOutcomes().size() == 3);
        });
        test("cancellation before dispatch and while queued has no side effects", [] {
            Fixture f;
            Gate inspect;
            auto calls = std::make_shared<std::atomic_int>(0);
            const auto work = [calls](const TaskInput &, const TaskControl &) { ++*calls; return TaskResult{}; };
            int applied = 0;
            const auto beforeDispatch = f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; });
            CHECK(f.coordinator->cancel(beforeDispatch) && f.coordinator->cancel(beforeDispatch));
            f.wait(1);
            CHECK(f.finished[0].disposition == TaskDisposition::Cancelled && calls->load() == 0 && applied == 0);
            CHECK(!f.coordinator->cancel(beforeDispatch));
            CHECK(f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "first\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; }) != 0);
            inspect.wait();
            const auto queued = f.coordinator->submitMutation("file", 1, work, changed,
                                                            [&](const TaskCompletion &) { ++applied; });
            CHECK(f.coordinator->cancel(queued));
            CHECK(until([&] { return f.finished.size() == 2; }));
            CHECK(f.finished[1].stamp.id == queued && f.finished[1].result.mutation == MutationObservation::NotAttempted);
            CHECK(calls->load() == 0 && applied == 0);
            inspect.open();
            f.wait(3);
        });
        test("cancellation wins admission while a worker is preparing a mutation", [] {
            Fixture f;
            Gate preparation;
            auto inspected = std::make_shared<std::atomic_bool>(false);
            const auto before = f.git({"ls-files", "--stage", "-z"});
            const auto id = f.coordinator->submitMutation("file", 1,
                [state = preparation.state](const TaskInput &input, const TaskControl &control) {
                    state->pause();
                    return writeResult(input, control, "must not be written\n");
                }, [inspected](const TaskInput &) { *inspected = true; return MutationObservation::ObservedChanged; });
            preparation.wait();
            CHECK(f.coordinator->cancel(id));
            preparation.open();
            f.wait(1);
            CHECK(!inspected->load() && f.finished[0].result.mutation == MutationObservation::NotAttempted);
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled && f.finished[0].disposition == TaskDisposition::Cancelled);
            CHECK(f.git({"ls-files", "--stage", "-z"}) == before);
            QFile file(f.root.path() + "/tracked.txt");
            CHECK(file.open(QIODevice::ReadOnly) && file.readAll() == "before\n");
        });
        test("cancellation after mutation retains inspection and holds the next writer", [] {
            Fixture f;
            Gate inspect;
            int applied = 0;
            auto nextRan = std::make_shared<std::atomic_bool>(false);
            const auto id = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "first\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; },
                [&](const TaskCompletion &) { ++applied; });
            inspect.wait();
            CHECK(f.coordinator->cancel(id));
            CHECK(f.coordinator->submitMutation("file", 1,
                [nextRan](const TaskInput &input, const TaskControl &control) {
                    *nextRan = true;
                    return writeResult(input, control, "next\n");
                }, changed) != 0);
            QCoreApplication::processEvents();
            CHECK(!nextRan->load() && f.finished.empty() && f.coordinator->pendingTasks() == 2);
            inspect.open();
            f.wait(2);
            CHECK(applied == 0 && nextRan->load());
            CHECK(f.finished[0].result.status == TaskStatus::Cancelled && f.finished[0].disposition == TaskDisposition::Cancelled);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged && f.finished[0].result.data == "first\n");
            CHECK(f.coordinator->mutationOutcomes()[0].stamp.id == id);
        });
        test("session replacement through a symlink retains an active worktree queue", [] {
            Fixture f;
            Gate inspect;
            QTemporaryDir aliases;
            CHECK(aliases.isValid() && QFile::link(f.root.path(), aliases.path() + "/alias"));
            auto nextRan = std::make_shared<std::atomic_bool>(false);
            const auto old = f.coordinator->submitMutation("file", 1,
                [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "old\n"); },
                [state = inspect.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; });
            inspect.wait();
            CHECK(f.coordinator->replaceSession(aliases.path() + "/alias", "next-merge", {FileSession("file", "tracked.txt", draft())}));
            const auto next = f.coordinator->submitMutation("file", 1,
                [nextRan](const TaskInput &input, const TaskControl &control) {
                    *nextRan = true;
                    return writeResult(input, control, "next\n");
                }, changed);
            CHECK(next != 0);
            QCoreApplication::processEvents();
            CHECK(!nextRan->load() && f.finished.empty());
            inspect.open();
            f.wait(2);
            CHECK(f.finished[0].stamp.id == old && f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[0].result.mutation == MutationObservation::ObservedChanged);
            CHECK(f.finished[1].stamp.id == next && f.finished[1].stamp.worktree == f.root.path());
            CHECK(f.finished[1].disposition == TaskDisposition::Current && nextRan->load());
        });
        test("worker and inspection failures retain honest mutation outcomes and release the queue", [] {
            for (int scenario = 0; scenario < 5; ++scenario) {
                Fixture f;
                int applied = 0;
                const auto id = f.coordinator->submitMutation("file", 1,
                    [scenario](const TaskInput &input, const TaskControl &control) {
                        if (scenario == 0) return TaskResult{};
                        auto result = writeResult(input, control, "written\n");
                        if (scenario == 1) throw std::runtime_error("worker failure after write");
                        return result;
                    }, [scenario](const TaskInput &) {
                        if (scenario == 2) throw std::runtime_error("inspection failure");
                        return scenario == 3 ? MutationObservation::NotAttempted
                            : scenario == 4 ? MutationObservation::Uncertain : MutationObservation::ObservedChanged;
                    }, [&](const TaskCompletion &value) { CHECK(value.result.status == TaskStatus::Failed); ++applied; });
                CHECK(id != 0);
                CHECK(f.coordinator->submitMutation("file", 1,
                    [](const TaskInput &input, const TaskControl &control) { return writeResult(input, control, "retry\n"); }, changed) != 0);
                f.wait(2);
                CHECK(applied == 1 && f.finished[0].result.status == TaskStatus::Failed);
                CHECK(f.finished[0].result.mutation == (scenario == 0 ? MutationObservation::NotAttempted
                    : scenario == 1 ? MutationObservation::ObservedChanged : MutationObservation::Uncertain));
                CHECK(!f.finished[0].result.diagnostic.isEmpty() && f.finished[1].result.status == TaskStatus::Succeeded);
                CHECK(f.coordinator->mutationOutcomes().size() == 2);
            }
        });
        test("teardown returns while workers or inspection are blocked and suppresses callbacks", [] {
            for (const bool duringInspection : {false, true}) {
                Fixture f;
                Gate gate;
                auto cancelled = std::make_shared<std::atomic_bool>(false);
                int applied = 0;
                const auto work = [state = gate.state, cancelled, duringInspection](const TaskInput &input, const TaskControl &control) {
                    if (duringInspection) return writeResult(input, control, "written\n");
                    state->pause();
                    *cancelled = control.cancellationRequested();
                    return TaskResult{};
                };
                if (duringInspection) {
                    CHECK(f.coordinator->submitMutation("file", 1, work,
                        [state = gate.state](const TaskInput &) { state->pause(); return MutationObservation::ObservedChanged; },
                        [&](const TaskCompletion &) { ++applied; }) != 0);
                } else {
                    CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; }) != 0);
                }
                gate.wait();
                QElapsedTimer timer;
                timer.start();
                f.coordinator.reset();
                CHECK(timer.elapsed() < 500);
                gate.open();
                CHECK(QThreadPool::globalInstance()->waitForDone(5000));
                QCoreApplication::processEvents();
                CHECK(f.finished.empty() && applied == 0 && (duringInspection || cancelled->load()));
            }
        });
        test("completion can replace a session and enqueue work before diagnostic notifications", [] {
            Fixture f;
            int applied = 0;
            bool replacementQueued = false;
            const auto work = [](const TaskInput &, const TaskControl &) { return TaskResult{}; };
            QObject::connect(f.coordinator.get(), &SessionCoordinator::taskFinished, &f.model,
                [&](const TaskCompletion &value) {
                    if (value.stamp.generation == 1) CHECK(applied == 1 && replacementQueued);
                });
            CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) {
                ++applied;
                f.replace("tracked.txt", "new-merge");
                CHECK(f.coordinator->submit("file", 1, work, [&](const TaskCompletion &) { ++applied; }) != 0);
                replacementQueued = true;
            }) != 0);
            f.wait(2);
            CHECK(applied == 2 && f.finished[0].disposition == TaskDisposition::Current);
            CHECK(f.finished[1].stamp.generation == 2);
        });
        test("pending notifications can cancel and replace without starting superseded work", [] {
            Fixture f;
            auto calls = std::make_shared<std::atomic_int>(0);
            bool replaced = false;
            bool resetGuarded = false;
            const auto work = [calls](const TaskInput &, const TaskControl &) { ++*calls; return TaskResult{}; };
            QObject::connect(&f.model, &QAbstractItemModel::modelAboutToBeReset, &f.model, [&] {
                resetGuarded = f.coordinator->submit("file", 1, work) == 0
                    && !f.coordinator->replaceSession(f.root.path(), "recursive", {});
            });
            QObject::connect(f.coordinator.get(), &SessionCoordinator::pendingTasksChanged, &f.model, [&] {
                if (replaced || f.coordinator->pendingTasks() != 1) return;
                replaced = true;
                f.replace("tracked.txt", "replacement");
                CHECK(f.coordinator->submit("file", 1, work) != 0);
            });
            CHECK(f.coordinator->submit("file", 1, work) != 0);
            f.wait(2);
            CHECK(replaced && resetGuarded && calls->load() == 1);
            CHECK(f.finished[0].disposition == TaskDisposition::StaleSession);
            CHECK(f.finished[1].disposition == TaskDisposition::Current);
        });
        test("callbacks and diagnostic listeners may destroy the coordinator", [] {
            for (const bool inCompletion : {false, true}) {
                Fixture f;
                bool destroyed = false;
                if (!inCompletion) {
                    QObject::connect(f.coordinator.get(), &SessionCoordinator::taskFinished, &f.model,
                        [&](const TaskCompletion &) { f.coordinator.reset(); destroyed = true; });
                }
                CHECK(f.coordinator->submit("file", 1, [](const TaskInput &, const TaskControl &) { return TaskResult{}; },
                    [&](const TaskCompletion &) { if (inCompletion) { f.coordinator.reset(); destroyed = true; } }) != 0);
                CHECK(until([&] { return destroyed; }));
                QCoreApplication::processEvents();
                CHECK(!f.coordinator && f.finished.size() == (inCompletion ? 0U : 1U));
            }
        });
        test("exact byte Git requests are admitted and handed off while different paths are rejected", [] {
            const QList<QByteArray> paths{"space\tand\nnewline", ":(glob)*", "--all", QByteArray("odd-") + char(0xff),
                                         QByteArray("odd-") + char(0xfe), QByteArray::fromHex("636166c3a9")};
            for (const auto &path : paths) {
                Fixture f;
                f.write(path, "edited\n");
                const auto beforeObject = f.git({"hash-object", "-w", "--stdin"}, "before\n").trimmed();
                f.git({"update-index", "-z", "--index-info"}, "100644 " + beforeObject + '\t' + path + '\0');
                const auto indexBefore = f.git({"ls-files", "--stage", "-z"});
                const auto expected = f.git({"hash-object", "--stdin"}, "edited\n").trimmed();
                f.replace(path);
                int applied = 0;
                git::Request request{git::Operation::StageFile, "/not-the-session", path + "different", {}, 999, 999};
                CHECK(f.coordinator->submitGit("file", 1, request) == 0);
                request.path = QString::fromUtf8(path).toUtf8();
                if (request.path != path) CHECK(f.coordinator->submitGit("file", 1, request) == 0);
                QCoreApplication::processEvents();
                CHECK(f.finished.empty() && f.coordinator->pendingTasks() == 0);
                CHECK(f.git({"ls-files", "--stage", "-z"}) == indexBefore);
                request.path = path;
                const auto id = f.coordinator->submitGit("file", 1, request, [&](const TaskCompletion &) { ++applied; });
                CHECK(id != 0);
                f.wait(1);
                CHECK(applied == 1 && f.finished[0].stamp.fileId == "file");
                CHECK(f.finished[0].result.gitResult.has_value());
                const auto &result = *f.finished[0].result.gitResult;
                CHECK(result.outcome == git::Outcome::Success && result.mutation == git::Mutation::ObservedChanged);
                CHECK(result.request.path == path && result.request.directory == f.root.path());
                CHECK(result.request.sessionGeneration == f.coordinator->generation() && result.request.inputRevision == 1);
                CHECK(result.indexAfter == "100644 " + expected + " 0\t" + path + QByteArray(1, '\0'));
                CHECK(f.coordinator->mutationOutcomes().size() == 1);
            }
        });
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir data;
    if (!data.isValid()) return 2;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    std::cout.setf(std::ios::unitbuf);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    return SessionCoordinatorTests{}.run();
}
