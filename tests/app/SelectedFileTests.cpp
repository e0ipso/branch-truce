// The selected file's projection: what the comparison panes bind to for the file the window shows.
#include "SessionFixtures.h"
#include "TestHarness.h"
#include "TestSession.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/SelectedFile.h"
#include "app/Selection.h"
#include "app/Translations.h"
#include "core/ExactLineCorrespondence.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"
#include "fixtures/Files.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/RecoveryJournal.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <atomic>
#include <stdexcept>
#include <vector>

using test_support::await;
using test_support::awaitAligned;
using test_support::commandResult;

using namespace branch_truce;
using namespace session_fixtures;

namespace {
// One line added by the left side, one earlier line removed by the right side
// and one line both sides changed. The result starts as the earlier version.
// The multibyte first line keeps UTF-16 offsets apart from byte offsets.
FileSession viewFile()
{
    const std::string earlier = "héader \U0001F600\nbridge one\nbase removed\nbridge two\nbase conflict\ntail\n";
    auto sources = *test_support::choiceFile(
                        "héader \U0001F600\nleft added\nbridge one\nbase removed\nbridge two\nleft conflict\ntail\n",
                        "héader \U0001F600\nbridge one\nbridge two\nright conflict\ntail\n")
                        .snapshots();
    sources.path = "src/view/choices.cpp";
    sources.earlier =
        SourceSnapshot{"earlier-object",        "100644", earlier, test_support::choiceText(earlier), "earlier-commit",
                       "Common earlier version"};
    sources.launchResult.text = test_support::choiceText(earlier);
    return FileSession("choices", sources.path, {earlier, {}, 0}, {.snapshots = sources, .initializeChoices = true});
}

// A deterministic alignment gate, armed only after the launch map exists.
class AlignmentGate final : public CorrespondenceAlgorithm {
public:
    mutable std::atomic_bool armed{false}, entered{false};
    mutable std::atomic_int comparisons{0};
    mutable QSemaphore release;
    std::string_view identity() const noexcept override { return ExactLineCorrespondence{}.identity(); }
    std::vector<Passage> compare(std::string_view source, std::string_view result) const override
    {
        ++comparisons;
        if (armed.exchange(false)) {
            entered = true;
            if (!release.tryAcquire(1, 5000)) throw std::bad_alloc();
        }
        return ExactLineCorrespondence().compare(source, result);
    }
};
struct ReleaseAlignment {
    std::shared_ptr<AlignmentGate> gate;
    ~ReleaseAlignment() { gate->release.release(); }
};

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

void checkRange(const ComparisonContribution &entry, const QString &prefix, const QString &text, int begin, int end)
{
    const bool source = prefix == QLatin1String("source");
    CHECK((source ? entry.sourceLineBegin : entry.resultLineBegin) == begin);
    CHECK((source ? entry.sourceLineEnd : entry.resultLineEnd) == end);
    CHECK((source ? entry.sourceUtf16Begin : entry.resultUtf16Begin) == lineOffset(text, begin));
    CHECK((source ? entry.sourceUtf16End : entry.resultUtf16End) == lineOffset(text, end));
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
        for (const auto &value : file->comparison()->passages()) {
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

// Every joined contribution exposes its command and range in the same value.
void checkRegions(const SelectedFile *file, const std::vector<ExpectedRegion> &expected)
{
    const auto regions = file->comparison()->contributions();
    CHECK(regions.size() == static_cast<qsizetype>(expected.size()));
    QSet<QString> ids;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto &region = regions[static_cast<qsizetype>(i)];
        const auto &want = expected[i];
        CHECK(!region.id.isEmpty() && !ids.contains(region.id));
        ids.insert(region.id);
        CHECK(region.side == QLatin1String(want.side));
        CHECK(region.scope == QLatin1String(want.scope));
        CHECK(region.state == QLatin1String(want.state));
        CHECK(region.mapped && !region.section.isEmpty());
        CHECK(region.decisionIds.size() == 1);
        checkRange(region, "source", file->property(want.side).toMap().value("text").toString(), want.sourceBegin,
                   want.sourceEnd);
        checkRange(region, "result", file->resultText(), want.resultBegin, want.resultEnd);
    }
}

QString regionId(const SelectedFile *file, const char *side, const char *scope, int sourceLine)
{
    for (const auto &value : file->comparison()->contributions()) {
        const auto &region = value;
        if (region.side == QLatin1String(side) && region.scope == QLatin1String(scope)
            && region.sourceLineBegin == sourceLine)
            return region.id;
    }
    throw std::runtime_error("fixture has no such region");
}

// The hand-edit flag of each region, in region order.
std::vector<bool> handEdits(const SelectedFile *file)
{
    std::vector<bool> result;
    for (const auto &value : file->comparison()->contributions()) {
        static_assert(std::is_same_v<decltype(value.handEdited), bool>);
        result.push_back(value.handEdited);
    }
    return result;
}

ComparisonContribution region(const SelectedFile *file, const QString &id)
{
    for (const auto &value : file->comparison()->contributions())
        if (value.id == id) return value;
    throw std::runtime_error("fixture has no such region");
}

QStringList regionStates(const SelectedFile *file)
{
    QStringList result;
    for (const auto &value : file->comparison()->contributions()) result.push_back(value.state);
    return result;
}

// viewFile() regions 6 to 9 cover the line both sides changed.
const std::vector<bool> &noHandEdits()
{
    static const std::vector<bool> value(10, false);
    return value;
}
const std::vector<bool> &conflictLineEdited()
{
    static const std::vector<bool> value{false, false, false, false, false, false, true, true, true, true};
    return value;
}

// One file in a controller, edited through its model and read back as the view sees it.
struct RegionView {
    explicit RegionView(FileSession file = viewFile())
    {
        CHECK(controller.store()->appendFile(std::move(file)));
        CHECK(controller.selection()->select("choices"));
    }
    RevisionId revision() { return controller.store()->file("choices")->current().id; }
    const std::string &text() { return controller.store()->file("choices")->current().draft.text; }
    SelectedFile *file() { return controller.selectedFile(); }
    SelectedFile *edit(ResultEdit edit)
    {
        CHECK(awaitAligned(*controller.store(), controller.store()->editResult("choices", revision(), std::move(edit)))
              == Transition::Applied);
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
        const auto preview = previewChoice(*controller.store()->file("choices"), revision(), id.toStdString(), action);
        CHECK(await(controller.store()->applyChoice("choices", preview, true)) == Transition::Applied);
        return file();
    }
    SelectedFile *undo()
    {
        CHECK(await(controller.store()->undo("choices", revision())) == Transition::Applied);
        return file();
    }
    SelectedFile *redo()
    {
        CHECK(await(controller.store()->redo("choices", revision())) == Transition::Applied);
        return file();
    }
    Application controller{test_support::windowConfig()};
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    test_support::Suite suite("SelectedFileTests");
    suite.run("comparison bindings receive one complete projection per mapping generation", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const NoSelectionGuard guard;
        Selection selection(rows, guard);
        SelectedFile selected(rows, selection);
        CHECK(selection.select(QStringLiteral("choices")));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("selected"), &selected);
        QQmlExpression binding(
            engine.rootContext(), nullptr,
            QStringLiteral(
                "JSON.stringify({passages:selected.comparison.passages, "
                "contributions:selected.comparison.contributions, "
                "handEditedLines:selected.comparison.handEditedLines, "
                "markerBlocks:selected.comparison.markerBlocks, markerOrphans:selected.comparison.markerOrphans, "
                "choiceNotice:selected.comparison.choiceNotice})"));
        binding.setNotifyOnValueChanged(true);
        const auto read = [&] {
            const auto value = binding.evaluate().toString();
            CHECK(!binding.hasError());
            return value;
        };
        const auto initial = read();
        QList<QPair<QString, QString>> observed, focused;
        QMap<QString, QString> publications;
        const auto snapshot = [&] { return qMakePair(selected.comparison()->generation(), read()); };
        QObject::connect(&binding, &QQmlExpression::valueChanged, [&] { observed.push_back(snapshot()); });
        const auto observeFocused = [&] { focused.push_back(snapshot()); };
        QObject::connect(selected.comparison(), &ComparisonPresentation::changed, [&] {
            const auto current = snapshot();
            CHECK(!publications.contains(current.first));
            publications.insert(current.first, current.second);
            observeFocused();
        });
        QObject::connect(selected.comparison(), &ComparisonPresentation::leftRowsChanged, observeFocused);
        QObject::connect(selected.comparison(), &ComparisonPresentation::rightRowsChanged, observeFocused);
        const auto checkPublication = [&] {
            const auto complete = read();
            CHECK(!publications.isEmpty());
            CHECK_EQ(observed.size(), publications.size());
            QSet<QString> generations;
            for (const auto &value : observed) {
                CHECK(!generations.contains(value.first));
                generations.insert(value.first);
                CHECK(publications.value(value.first) == value.second);
            }
            CHECK(observed.back().second == complete);
            CHECK(focused.size() >= publications.size());
            for (const auto &value : focused) CHECK(publications.value(value.first) == value.second);
            observed.clear();
            focused.clear();
            publications.clear();
            return complete;
        };
        CHECK(awaitAligned(
                  store, store.editResult("choices", store.file("choices")->current().id, {{0, 0}, "manual prefix\n"}))
              == Transition::Applied);
        selected.sync();
        CHECK(checkPublication() != initial);
        // Status-only refreshes must not recompute the same QML projection.
        selected.sync();
        CHECK(observed.isEmpty() && focused.isEmpty());
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        selected.sync();
        CHECK(checkPublication() == initial);
        CHECK(await(store.redo("choices", store.file("choices")->current().id)) == Transition::Applied);
        selected.sync();
        CHECK(checkPublication() != initial);
        // Without files nothing is selected and the projection empties.
        store.replaceFiles({});
        checkPublication();
        CHECK(selected.comparison()->passages().isEmpty() && selected.comparison()->contributions().isEmpty()
              && selected.comparison()->handEditedLines().isEmpty() && selected.comparison()->leftRows().isEmpty()
              && selected.comparison()->rightRows().isEmpty());
    });
    suite.run("background alignment refreshes the selected comparison at the same revision", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::headingFile()));
        test_support::awaitRecovery(store);
        const NoSelectionGuard guard;
        Selection selection(rows, guard);
        SelectedFile selected(rows, selection);
        CHECK(selection.select(QStringLiteral("choices")));
        CHECK(store.typeResult("choices", 1, {{6, 8}, "S\n"}) == Transition::Applied);
        const auto *file = store.file("choices");
        const auto pending = file->pendingAlignment();
        CHECK(pending && selected.comparison()->markerOrphans().size() == 1);
        const auto passages = selected.comparison()->passages();
        const auto revision = selected.currentRevision();
        QSignalSpy comparison(selected.comparison(), &ComparisonPresentation::changed);
        CHECK(comparison.isValid());
        CHECK(store.installAlignment("choices", file->current().id,
                                     std::make_shared<const CorrespondenceMap>(pending->realigned()))
              == Transition::Applied);
        CHECK(selected.currentRevision() == revision);
        CHECK(selected.comparison()->passages() != passages && selected.comparison()->markerOrphans().isEmpty());
        CHECK(comparison.count() == 1);
        selected.sync();
        CHECK(comparison.count() == 1);
    });
    suite.run("the selected file exposes passage kinds, region ranges, decisions and the file name", [] {
        Application controller{test_support::windowConfig()};
        CHECK(controller.store()->appendFile(viewFile()));
        CHECK(controller.selection()->select("choices"));
        const auto file = controller.selectedFile();
        CHECK(file->fileName() == "choices.cpp" && file->path() == "src/view/choices.cpp");
        CHECK(file->pathBytes() == "src/view/choices.cpp" && file->currentRevision() == "1");
        CHECK(file->decisions()
              == controller.files()->data(controller.files()->index(0), SessionModel::DecisionsRole).toList());
        CHECK(file->decisions().size() == 6);
        checkPassages(file,
                      {{"stage2", "source-only", 2, 3, 2, 2},
                       {"stage2", "changed", 6, 7, 5, 6},
                       {"stage3", "result-only", 3, 3, 3, 4},
                       {"stage3", "changed", 4, 5, 5, 6}});
        checkRegions(file,
                     {{"stage2", "section", "undecided", 2, 3, 2, 2},
                      {"stage2", "line", "undecided", 2, 3, 2, 2},
                      {"stage3", "section", "undecided", 2, 2, 2, 2},
                      {"stage2", "section", "undecided", 4, 5, 3, 4},
                      {"stage2", "line", "undecided", 4, 5, 3, 4},
                      {"stage3", "section", "undecided", 3, 3, 4, 4},
                      {"stage2", "section", "undecided", 6, 7, 5, 6},
                      {"stage2", "line", "undecided", 6, 7, 5, 6},
                      {"stage3", "section", "undecided", 4, 5, 5, 6},
                      {"stage3", "line", "undecided", 4, 5, 5, 6}});
        const auto choices = file->comparison()->contributions();
        CHECK(choices[6].label == QString::fromUtf8("Left section, lines 6–6"));
        CHECK(choices[6].decisionState == "undecided");
    });
    suite.run("selected file notifies changed parts without re-emitting sources on edits", [] {
        Application controller{test_support::windowConfig()};
        CHECK(controller.store()->appendFile(viewFile()));
        CHECK(controller.selection()->select("choices"));
        auto *selected = controller.selectedFile();
        QSignalSpy identity(selected, &SelectedFile::identityChanged);
        QSignalSpy sources(selected, &SelectedFile::sourcesChanged);
        QSignalSpy result(selected, &SelectedFile::resultTextChanged);
        QSignalSpy decisions(selected, &SelectedFile::decisionsChanged);
        CHECK(identity.isValid() && sources.isValid() && result.isValid() && decisions.isValid());
        const auto source = selected->stage2();
        const auto revision = [&] { return controller.store()->file("choices")->current().id; };
        CHECK(
            awaitAligned(*controller.store(), controller.store()->editResult("choices", revision(), {{0, 0}, "typed "}))
            == Transition::Applied);
        CHECK(controller.selectedFile() == selected);
        CHECK(identity.count() == 0 && sources.count() == 0 && result.count() == 1 && decisions.count() == 0);
        CHECK(selected->stage2() == source && selected->resultText().startsWith("typed "));
        CHECK(selected->currentRevision()
              == controller.files()->data(controller.files()->index(0), SessionModel::CurrentRevisionRole).toString());
        const auto target = regionId(selected, "stage2", "line", 6);
        const auto preview =
            previewChoice(*controller.store()->file("choices"), revision(), target.toStdString(), ChoiceAction::Ignore);
        CHECK(await(controller.store()->applyChoice("choices", preview)) == Transition::Applied);
        CHECK(identity.count() == 0 && sources.count() == 0 && result.count() == 1 && decisions.count() == 1);
    });
    suite.run("accepted controller edit keeps source signals and row status in sync", [] {
        Application controller{test_support::windowConfig()};
        CHECK(controller.store()->appendFile(viewFile()));
        test_support::awaitRecovery(*controller.store());
        CHECK(controller.selection()->select("choices"));
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
        CHECK(sources.isValid() && identity.isValid() && result.isValid() && status.isValid() && rows.isValid());
        CHECK(commandResult(
            controller, controller.commands()->editResult("choices", originalRevision, 0, 0, QString::fromUtf8("é "))));
        CHECK(controller.selectedFile() == selected && selected->stage2() == originalSource);
        CHECK(sources.count() == 0 && identity.count() == 0 && result.count() == 1);
        // Admission, durable revision and completed alignment each refresh the row.
        EXPECT_EQ(status.count(), 2);
        EXPECT_EQ(rows.count(), 3);
        CHECK(selected->resultText() == QString::fromUtf8("é ") + originalText);
        CHECK(controller.store()->file("choices")->current().draft.text
              == selected->resultText().toUtf8().toStdString());
        CHECK(selected->currentRevision() == row.data(SessionModel::CurrentRevisionRole).toString());
        CHECK(selected->status() == row.data(SessionModel::StatusRole).toString());
        CHECK(selected->progress() == row.data(SessionModel::ProgressRole).toMap());
        CHECK(
            !commandResult(controller, controller.commands()->editResult("choices", originalRevision, 0, 0, "stale")));
        CHECK(sources.count() == 0 && identity.count() == 0 && result.count() == 1 && status.count() == 2
              && rows.count() == 3);
    });
    suite.run("selected file ranges, kinds and region states follow each new revision", [] {
        Application controller{test_support::windowConfig()};
        CHECK(controller.store()->appendFile(viewFile()));
        CHECK(controller.selection()->select("choices"));
        int changes = 0;
        QObject::connect(controller.selectedFile(), &SelectedFile::statusChanged, [&] { ++changes; });
        const auto revision = [&] { return controller.store()->file("choices")->current().id; };

        CHECK(awaitAligned(*controller.store(),
                           controller.store()->editResult("choices", revision(), {{0, 0}, "inserted é\U0001F600\n"}))
              == Transition::Applied);
        auto file = controller.selectedFile();
        CHECK(changes > 0 && file->currentRevision() == "2");
        checkPassages(file,
                      {{"stage2", "result-only", 1, 1, 1, 2},
                       {"stage2", "source-only", 2, 3, 3, 3},
                       {"stage2", "changed", 6, 7, 6, 7},
                       {"stage3", "result-only", 1, 1, 1, 2},
                       {"stage3", "result-only", 3, 3, 4, 5},
                       {"stage3", "changed", 4, 5, 6, 7},
                       {"earlier", "result-only", 1, 1, 1, 2}});
        checkRegions(file,
                     {{"stage2", "section", "undecided", 2, 3, 3, 3},
                      {"stage2", "line", "undecided", 2, 3, 3, 3},
                      {"stage3", "section", "undecided", 2, 2, 3, 3},
                      {"stage2", "section", "undecided", 4, 5, 4, 5},
                      {"stage2", "line", "undecided", 4, 5, 4, 5},
                      {"stage3", "section", "undecided", 3, 3, 5, 5},
                      {"stage2", "section", "undecided", 6, 7, 6, 7},
                      {"stage2", "line", "undecided", 6, 7, 6, 7},
                      {"stage3", "section", "undecided", 4, 5, 6, 7},
                      {"stage3", "line", "undecided", 4, 5, 6, 7}});

        const auto choose = [&](const QString &id, ChoiceAction action) {
            const auto preview =
                previewChoice(*controller.store()->file("choices"), revision(), id.toStdString(), action);
            CHECK(await(controller.store()->applyChoice("choices", preview, true)) == Transition::Applied);
        };
        changes = 0;
        choose(regionId(file, "stage2", "line", 6), ChoiceAction::Accept);
        choose(regionId(file, "stage2", "line", 2), ChoiceAction::Ignore);
        file = controller.selectedFile();
        CHECK(changes > 0 && file->currentRevision() == "4");
        CHECK(file->decisions()
              == controller.files()->data(controller.files()->index(0), SessionModel::DecisionsRole).toList());
        // The accepted left line now equals the result, so only the right side still differs there.
        checkPassages(file,
                      {{"stage2", "result-only", 1, 1, 1, 2},
                       {"stage2", "source-only", 2, 3, 3, 3},
                       {"stage3", "result-only", 1, 1, 1, 2},
                       {"stage3", "result-only", 3, 3, 4, 5},
                       {"stage3", "changed", 4, 5, 6, 7},
                       {"earlier", "result-only", 1, 1, 1, 2},
                       {"earlier", "changed", 5, 6, 6, 7}});
        checkRegions(file,
                     {{"stage2", "section", "ignored", 2, 3, 3, 3},
                      {"stage2", "line", "ignored", 2, 3, 3, 3},
                      {"stage3", "section", "undecided", 2, 2, 3, 3},
                      {"stage2", "section", "undecided", 4, 5, 4, 5},
                      {"stage2", "line", "undecided", 4, 5, 4, 5},
                      {"stage3", "section", "undecided", 3, 3, 5, 5},
                      {"stage2", "section", "accepted", 6, 7, 6, 7},
                      {"stage2", "line", "accepted", 6, 7, 6, 7},
                      {"stage3", "section", "undecided", 4, 5, 6, 7},
                      {"stage3", "line", "undecided", 4, 5, 6, 7}});

        // A hand edit inside accepted text marks it manual and leaves the undecided competitor alone.
        const auto at = controller.store()->file("choices")->current().draft.text.find("left conflict") + 5;
        CHECK(awaitAligned(*controller.store(),
                           controller.store()->editResult("choices", revision(), {{at, at}, "hand "}))
              == Transition::Applied);
        file = controller.selectedFile();
        CHECK(file->currentRevision() == "5");
        const auto regions = file->comparison()->contributions();
        CHECK(regions[6].state == "manual" && regions[7].state == "manual");
        CHECK(regions[8].state == "undecided" && regions[9].state == "undecided");
        checkRange(regions[6], "result", file->resultText(), 6, 7);
        CHECK(await(controller.store()->undo("choices", revision())) == Transition::Applied);
        file = controller.selectedFile();
        CHECK(file->comparison()->contributions()[6].state == "accepted");
        CHECK(file->resultText().contains("\nleft conflict\n"));
    });
    suite.run("no region reports a hand edit at launch, even where the result already differs", [] {
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
            for (const auto &value : file->comparison()->contributions()) {
                const auto *anchor =
                    view.controller.store()->file("choices")->correspondence()->anchor(value.id.toStdString());
                if (anchor && anchor->manualOverlap) ++overlapping;
            }
        }
        CHECK(overlapping > 0);
    });
    suite.run("deciding both sides of a marked section removes its marker lines from the shown result", [] {
        RegionView view(test_support::markedFile());
        auto file = view.file();
        const auto preview = [&](const char *side, ChoicePreviewFlow::Action action) {
            return test_support::previewResult(
                view.controller,
                view.controller.choices()->previewChoice("choices", file->currentRevision(),
                                                         regionId(file, side, "section", 2), action));
        };
        CHECK(preview("stage2", ChoicePreviewFlow::Action::Accept));
        CHECK(view.controller.feedback()->message()
              == "Accept changes only this scope and leaves the other side undecided.");
        CHECK(commandResult(view.controller, view.controller.choices()->applyChoice()));
        file = view.file();
        CHECK(file->resultText() == QString::fromStdString(test_support::markedText()));
        CHECK(preview("stage3", ChoicePreviewFlow::Action::Ignore));
        CHECK(view.controller.feedback()->message().contains("marker lines and ignored text are removed"));
        CHECK(commandResult(view.controller, view.controller.choices()->applyChoice()));
        file = view.file();
        const auto resolved = QStringLiteral("header\nleft first\nleft second\nbridge\n"
                                             "<<<<<<< ours\nleft later\n=======\nright later\n>>>>>>> theirs\ntail\n");
        CHECK(file->resultText() == resolved);
        CHECK(region(file, regionId(file, "stage2", "section", 2)).state == "accepted");
        CHECK(view.controller.store()->file("choices")->current().draft.unresolvedMarkers == 1);
        CHECK(await(view.undo())->resultText() == QString::fromStdString(test_support::markedText()));
        CHECK(await(view.redo())->resultText() == resolved);
    });
    suite.run("a hand edit marks the regions containing it in every decision state until a choice replaces it", [] {
        RegionView view;
        auto file = view.file();
        const auto leftConflict = regionId(file, "stage2", "line", 6);
        const auto rightConflict = regionId(file, "stage3", "line", 4);
        const auto removed = regionId(file, "stage2", "line", 4);
        const auto deletion = regionId(file, "stage3", "section", 3);
        auto expected = QStringList(10, "undecided");

        // Undecided regions report the edit and stay undecided.
        file = view.type("base conflict", 5, "hand ");
        CHECK(handEdits(file) == conflictLineEdited() && regionStates(file) == expected);
        file = view.choose(leftConflict, ChoiceAction::Accept);
        expected[6] = expected[7] = "accepted";
        CHECK(handEdits(file) == noHandEdits() && regionStates(file) == expected);

        // Accepted.
        file = view.type("left conflict", 5, "hand ");
        expected[6] = expected[7] = "manual";
        CHECK(handEdits(file) == conflictLineEdited() && regionStates(file) == expected);
        file = view.choose(rightConflict, ChoiceAction::CombineLeftFirst);
        for (const int i : {6, 7, 8, 9}) expected[i] = "accepted";
        CHECK(handEdits(file) == noHandEdits() && regionStates(file) == expected);
        CHECK(file->resultText().contains("\nleft conflict\nright conflict\n"));

        // Combined.
        file = view.type("right conflict", 6, "hand ");
        for (const int i : {6, 7, 8, 9}) expected[i] = "manual";
        CHECK(handEdits(file) == conflictLineEdited() && regionStates(file) == expected);
        file = view.choose(leftConflict, ChoiceAction::CombineRightFirst);
        for (const int i : {6, 7, 8, 9}) expected[i] = "accepted";
        CHECK(handEdits(file) == noHandEdits() && regionStates(file) == expected);
        CHECK(file->resultText().contains("\nright conflict\nleft conflict\n"));

        // Ignored. Ignoring keeps the text, so only accepting the deletion replaces the edit.
        file = view.choose(removed, ChoiceAction::Ignore);
        expected[3] = expected[4] = "ignored";
        CHECK(handEdits(file) == noHandEdits() && regionStates(file) == expected);
        file = view.type("base removed", 5, "hand ");
        expected[3] = expected[4] = "manual";
        CHECK(handEdits(file) == std::vector<bool>{false, false, false, true, true, false, false, false, false, false});
        CHECK(regionStates(file) == expected);
        file = view.choose(deletion, ChoiceAction::Accept);
        expected[3] = expected[4] = "ignored";
        expected[5] = "accepted";
        CHECK(handEdits(file) == noHandEdits() && regionStates(file) == expected);
        CHECK(!file->resultText().contains("removed"));
    });
    suite.run("typing across a region boundary marks it and a whole-file choice clears every hand edit", [] {
        RegionView crossing;
        auto file = crossing.choose(regionId(crossing.file(), "stage2", "line", 6), ChoiceAction::Accept);
        const auto at = crossing.text().find("two\nleft conflict");
        CHECK(at != std::string::npos);
        file = crossing.edit({{at, at + 8}, "2\nright"});
        const auto regions = file->comparison()->contributions();
        CHECK(handEdits(file) == conflictLineEdited() && !regions[6].mapped);
        CHECK(regionStates(file)[6] == "manual" && regionStates(file)[8] == "undecided");

        RegionView view;
        view.type("base conflict", 5, "hand ");
        file = view.type("base removed", 5, "hand ");
        CHECK(handEdits(file) == std::vector<bool>{false, false, false, true, true, false, true, true, true, true});
        auto &files = *view.controller.store();
        const auto preview = previewWholeFile(*files.file("choices"), view.revision(), SourceSide::Stage2);
        CHECK(await(files.applyWholeFile("choices", preview, true)) == Transition::Applied);
        file = view.file();
        CHECK(handEdits(file) == noHandEdits());
        CHECK(!regionStates(file).contains("manual") && !regionStates(file).contains("undecided"));
    });
    suite.run("undo and redo restore each region's hand-edit flag with the text and decisions", [] {
        RegionView view;
        const auto leftConflict = regionId(view.file(), "stage2", "line", 6);
        auto file = view.type("base conflict", 5, "hand ");
        CHECK(handEdits(file) == conflictLineEdited());
        file = view.choose(leftConflict, ChoiceAction::Accept);
        CHECK(handEdits(file) == noHandEdits() && regionStates(file)[6] == "accepted");
        file = await(view.undo());
        CHECK(handEdits(file) == conflictLineEdited() && regionStates(file)[6] == "undecided");
        CHECK(file->resultText().contains("\nbase hand conflict\n"));
        file = await(view.undo());
        CHECK(handEdits(file) == noHandEdits() && file->resultText().contains("\nbase conflict\n"));
        file = await(view.redo());
        CHECK(handEdits(file) == conflictLineEdited() && file->resultText().contains("\nbase hand conflict\n"));
        file = await(view.redo());
        CHECK(handEdits(file) == noHandEdits() && regionStates(file)[6] == "accepted");
        CHECK(file->resultText().contains("\nleft conflict\n"));
    });
    suite.run("a choice clears hand edits on a deleted line it restores and on text an edit left unchanged", [] {
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
        CHECK(region(file, line).handEdited && !region(file, line).mapped);
        file = deleted.choose(regionId(file, "stage2", "section", 2), ChoiceAction::Accept);
        CHECK(deleted.text() == left && none(file) && region(file, line).state == "accepted");
        file = deleted.undo();
        CHECK(region(file, line).handEdited && region(file, line).state == "undecided");
        file = deleted.redo();
        CHECK(none(file) && region(file, line).state == "accepted");

        RegionView unchanged(test_support::choiceFile());
        auto &files = *unchanged.controller.store();
        const auto wholeLeft = [&] {
            return await(files.applyWholeFile(
                "choices", previewWholeFile(*files.file("choices"), unchanged.revision(), SourceSide::Stage2), true));
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
    suite.run("recovery replay restores hand edits cleared over a deleted line or unchanged text", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        const auto revision = [&] { return store.file("choices")->current().id; };
        const auto edit = [&](ResultEdit value) {
            CHECK(awaitAligned(store, store.editResult("choices", revision(), std::move(value)))
                  == Transition::Applied);
        };
        const auto replayed = [&] {
            const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.size() == 1);
            CHECK(replay.files.front().current().draft == store.file("choices")->current().draft);
            RegionView live(*store.file("choices"));
            RegionView restored(replay.files.front());
            CHECK(handEdits(restored.file()) == handEdits(live.file()));
            CHECK(regionStates(restored.file()) == regionStates(live.file()));
            return handEdits(restored.file());
        };
        const auto cleared = std::vector<bool>(handEdits(RegionView(test_support::choiceFile()).file()).size(), false);

        const auto at = store.file("choices")->current().draft.text.find("left first\n");
        edit({{at, at + 11}, ""});
        const auto deleted = replayed();
        const auto section =
            test_support::choiceTarget(*store.file("choices"), SourceSide::Stage2, ChoiceScope::Section)
                .source.contributionId;
        CHECK(await(store.applyChoice(
                  "choices", previewChoice(*store.file("choices"), revision(), section, ChoiceAction::Accept), true))
              == Transition::Applied);
        CHECK(replayed() == cleared);
        CHECK(await(store.undo("choices", revision())) == Transition::Applied);
        CHECK(replayed() == deleted);
        CHECK(await(store.redo("choices", revision())) == Transition::Applied);
        CHECK(replayed() == cleared);

        const auto wholeLeft = [&] {
            return await(store.applyWholeFile(
                "choices", previewWholeFile(*store.file("choices"), revision(), SourceSide::Stage2), true));
        };
        CHECK(wholeLeft() == Transition::Applied);
        const auto typed = store.file("choices")->current().draft.text.find("left first") + 5;
        edit({{typed, typed}, "X"});
        edit({{typed, typed + 1}, ""});
        const auto manual = replayed();
        CHECK(manual != cleared);
        // This record repeats the previous draft and differs only in its hand edits.
        CHECK(wholeLeft() == Transition::Applied);
        CHECK(replayed() == cleared);
        CHECK(await(store.undo("choices", revision())) == Transition::Applied);
        CHECK(replayed() == manual);
        CHECK(await(store.redo("choices", revision())) == Transition::Applied);
        CHECK(replayed() == cleared);
    });
    suite.run("recovery replay restores each region's hand-edit flag with the text and decisions", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(viewFile()));
        test_support::awaitRecovery(store);
        const auto revision = [&] { return store.file("choices")->current().id; };
        const auto type = [&](const std::string &near, const std::string &typed) {
            const auto at = store.file("choices")->current().draft.text.find(near) + 5;
            CHECK(awaitAligned(store, store.editResult("choices", revision(), {{at, at}, typed}))
                  == Transition::Applied);
        };
        // A fresh replay of the journal shows the same regions as the live model.
        const auto replayed = [&] {
            const auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
            CHECK(replay.succeeded && replay.files.size() == 1);
            CHECK(replay.files.front().current().draft == store.file("choices")->current().draft);
            RegionView live(*store.file("choices"));
            RegionView restored(replay.files.front());
            const auto file = restored.file();
            CHECK(handEdits(file) == handEdits(live.file()) && regionStates(file) == regionStates(live.file()));
            return std::make_pair(handEdits(file), regionStates(file));
        };
        type("base conflict", "hand ");
        CHECK(replayed().first == conflictLineEdited());
        const auto leftConflict =
            test_support::choiceTarget(*store.file("choices"), SourceSide::Stage2, ChoiceScope::Line, 2)
                .source.contributionId;
        CHECK(
            await(store.applyChoice(
                "choices", previewChoice(*store.file("choices"), revision(), leftConflict, ChoiceAction::Accept), true))
            == Transition::Applied);
        // Replay applies the choice as a whole-draft replacement, which must not count as typing.
        auto snapshot = replayed();
        CHECK(snapshot.first == noHandEdits() && snapshot.second[6] == "accepted");
        type("left conflict", "hand ");
        type("base removed", "hand ");
        CHECK(await(store.undo("choices", revision())) == Transition::Applied);
        snapshot = replayed();
        CHECK(snapshot.first == conflictLineEdited() && snapshot.second[6] == "manual");
        CHECK(await(store.redo("choices", revision())) == Transition::Applied);
        snapshot = replayed();
        CHECK(snapshot.first == std::vector<bool>{false, false, false, true, true, false, true, true, true, true});
    });
    suite.run("records must list their hand edits, and unknown anchors are rejected", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(viewFile()));
        test_support::awaitRecovery(store);
        const auto at = store.file("choices")->current().draft.text.find("base conflict") + 5;
        CHECK(awaitAligned(store, store.editResult("choices", 1, {{at, at}, "hand "})) == Transition::Applied);
        const auto recorded = store.file("choices")->correspondence()->handEditedAnchors();
        CHECK(recorded.size() == 4);
        auto replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(replay.succeeded && replay.files.size() == 1);
        CHECK(replay.files.front().correspondence()->handEditedAnchors() == recorded);
        // A record without the field is refused rather than read as having none.
        const auto journal = test_support::readFile(store.recoveryDirectory() + "/journal");
        test_support::rewriteLastRecoveryState(store.recoveryDirectory(), [&](QJsonObject &state) {
            auto current = state.value("current").toObject();
            CHECK(current.value("handEdited").toArray().size() == static_cast<qsizetype>(recorded.size()));
            CHECK(current.value("handEditedRanges").toArray().size() == 1);
            current.remove("handEdited");
            state.insert("current", current);
        });
        replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(!replay.succeeded && replay.problem.arguments.value(0).contains("hand edits"));
        // Every recorded hand edit must name an anchor of the replayed draft.
        test_support::writeFile(store.recoveryDirectory() + "/journal", journal);
        test_support::rewriteLastRecoveryState(store.recoveryDirectory(), [](QJsonObject &state) {
            auto current = state.value("current").toObject();
            current.insert("handEdited", QJsonArray{state.value("path")});
            state.insert("current", current);
        });
        replay = replayRecovery(storage::RecoveryStore(store.recoveryDirectory()));
        CHECK(!replay.succeeded && replay.files.empty());
    });
    for (const auto replacement : {"none", "revision", "session", "selection", "action", "cancel"}) {
        suite.run(std::string("a preview awaits one alignment and fences a pending ") + replacement, [=] {
            Application controller(test_support::windowConfig());
            auto algorithm = std::make_shared<AlignmentGate>();
            auto sources = *test_support::choiceFile().snapshots();
            const auto text = sources.launchResult.text.utf8;
            CHECK(controller.store()->appendFile(
                FileSession("choices", sources.path, {text, {}, 0},
                            {.snapshots = sources, .initializeChoices = true, .algorithm = algorithm})));
            CHECK(controller.selection()->select("choices"));
            algorithm->armed = true;
            ReleaseAlignment release{algorithm};
            CHECK(controller.store()->typeResult("choices", 1, {{0, 0}, "typed "}) == Transition::Applied);
            test_support::waitUntil([&] { return algorithm->entered.load(); }, "alignment did not reach the gate");
            const auto comparisons = algorithm->comparisons.load();
            auto *choices = controller.choices();
            QSignalSpy ready(choices, &ChoicePreviewFlow::ready);
            CHECK(choices->previewWholeFile("choices", "2", ChoicePreviewFlow::Side::RightSide));
            CHECK(choices->previewPending() && choices->wholeFilePreview().isEmpty() && ready.empty());
            CHECK(algorithm->comparisons == comparisons);
            const std::string change(replacement);
            if (change == "revision")
                CHECK(controller.store()->typeResult("choices", 2, {{0, 0}, "newer "}) == Transition::Applied);
            else if (change == "session") {
                controller.store()->recordTyping();
                test_support::waitUntil([&] { return !controller.store()->recording(); }, "typing did not commit");
                controller.store()->replaceFiles({test_support::choiceFile()});
            } else if (change == "selection") {
                CHECK(controller.store()->appendFile(FileSession("other", "other.txt", {})));
                CHECK(controller.selection()->select("other"));
            } else if (change == "action") {
                CHECK(choices->previewWholeFile("choices", "2", ChoicePreviewFlow::Side::LeftSide));
            } else if (change == "cancel") {
                choices->cancel();
            }
            algorithm->release.release();
            if (change == "none" || change == "action") {
                test_support::waitUntil([&] { return ready.size() == 1; }, "the requested preview was not delivered");
                CHECK(!choices->previewPending());
                CHECK(choices->wholeFilePreview().value("side").toString() == (change == "none" ? "Right" : "Left"));
                CHECK(controller.store()->file("choices")->current().id == 2);
            } else {
                test_support::waitUntil([&] { return !choices->previewPending(); }, "stale preview remained pending");
                await(controller.store()->awaitAlignment("choices", controller.store()->file("choices")->current().id));
                QCoreApplication::processEvents();
                CHECK(ready.empty() && choices->wholeFilePreview().isEmpty());
            }
        });
    }
    return suite.report();
}
