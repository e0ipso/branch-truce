// The selected file's projection: what the comparison panes bind to for the file the window shows.
#include "SessionFixtures.h"
#include "TestHarness.h"
#include "TestSession.h"
#include "app/AppPaths.h"
#include "app/Application.h"
#include "app/SelectedFile.h"
#include "app/Selection.h"
#include "app/Translations.h"
#include "fixtures/Files.h"
#include "fixtures/GitOnPath.h"
#include "fixtures/RecoveryJournal.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <stdexcept>
#include <vector>

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
        checkRange(region, "source", file->property(want.side).toMap().value("text").toString(), want.sourceBegin,
                   want.sourceEnd);
        checkRange(region, "result", file->resultText(), want.resultBegin, want.resultEnd);
    }
}

QString regionId(const SelectedFile *file, const char *side, const char *scope, int sourceLine)
{
    for (const auto &value : file->regions()) {
        const auto region = value.toMap();
        if (region.value("side").toString() == QLatin1String(side)
            && region.value("scope").toString() == QLatin1String(scope)
            && region.value("sourceLineBegin").toInt() == sourceLine)
            return region.value("id").toString();
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
        CHECK(controller.store()->appendFile(std::move(file)));
        CHECK(controller.selection()->select("choices"));
    }
    RevisionId revision() { return controller.store()->file("choices")->current().id; }
    const std::string &text() { return controller.store()->file("choices")->current().draft.text; }
    SelectedFile *file() { return controller.selectedFile(); }
    SelectedFile *edit(ResultEdit edit)
    {
        CHECK(controller.store()->editResult("choices", revision(), std::move(edit)) == Transition::Applied);
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
        CHECK(controller.store()->applyChoice("choices", preview, true) == Transition::Applied);
        return file();
    }
    SelectedFile *undo()
    {
        CHECK(controller.store()->undo("choices", revision()) == Transition::Applied);
        return file();
    }
    SelectedFile *redo()
    {
        CHECK(controller.store()->redo("choices", revision()) == Transition::Applied);
        return file();
    }
    Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    test_support::Suite suite("SelectedFileTests");
    suite.run("comparison bindings receive one complete projection per revision", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::choiceFile()));
        const NoSelectionGuard guard;
        Selection selection(model.rows, guard);
        SelectedFile selected(model.rows, selection);
        CHECK(selection.select(QStringLiteral("choices")));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("selected"), &selected);
        QQmlExpression binding(
            engine.rootContext(), nullptr,
            QStringLiteral("JSON.stringify({passages:selected.passages, regions:selected.regions, "
                           "handEditedLines:selected.handEditedLines, choices:selected.choices, "
                           "markerBlocks:selected.markerBlocks, markerOrphans:selected.markerOrphans, "
                           "choiceNotice:selected.choiceNotice})"));
        binding.setNotifyOnValueChanged(true);
        const auto read = [&] {
            const auto value = binding.evaluate().toString();
            CHECK(!binding.hasError());
            return value;
        };
        const auto initial = read();
        QStringList observed, focused;
        QObject::connect(&binding, &QQmlExpression::valueChanged, [&] { observed.push_back(read()); });
        const auto observeFocused = [&] { focused.push_back(read()); };
        QObject::connect(&selected, &SelectedFile::passagesChanged, observeFocused);
        QObject::connect(&selected, &SelectedFile::regionsChanged, observeFocused);
        QObject::connect(&selected, &SelectedFile::handEditedLinesChanged, observeFocused);
        const auto checkPublication = [&] {
            const auto complete = read();
            CHECK(observed.size() == 1);
            CHECK(observed.front() == complete);
            CHECK(focused.size() >= 2);
            for (const auto &value : focused) CHECK(value == complete);
            observed.clear();
            focused.clear();
            return complete;
        };
        CHECK(model.editResult("choices", model.file("choices")->current().id, {{0, 0}, "manual prefix\n"})
              == Transition::Applied);
        selected.sync();
        CHECK(checkPublication() != initial);
        // Status-only refreshes must not recompute the same QML projection.
        selected.sync();
        CHECK(observed.isEmpty() && focused.isEmpty());
        CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
        selected.sync();
        CHECK(checkPublication() == initial);
        CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
        selected.sync();
        CHECK(checkPublication() != initial);
        // Without files nothing is selected and the projection empties.
        model.replaceFiles({});
        checkPublication();
        CHECK(selected.passages().isEmpty() && selected.regions().isEmpty() && selected.handEditedLines().isEmpty()
              && selected.choices().isEmpty());
    });
    suite.run("the selected file exposes passage kinds, region ranges, decisions and the file name", [] {
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
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
        const auto choices = file->choices();
        CHECK(choices[6].toMap().value("label").toString() == QString::fromUtf8("Left section, lines 6–6"));
        CHECK(choices[6].toMap().value("state").toString() == "undecided");
    });
    suite.run("selected file notifies changed parts without re-emitting sources on edits", [] {
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
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
        CHECK(controller.store()->editResult("choices", revision(), {{0, 0}, "typed "}) == Transition::Applied);
        CHECK(controller.selectedFile() == selected);
        CHECK(identity.count() == 0 && sources.count() == 0 && result.count() == 1 && decisions.count() == 0);
        CHECK(selected->stage2() == source && selected->resultText().startsWith("typed "));
        CHECK(selected->currentRevision()
              == controller.files()->data(controller.files()->index(0), SessionModel::CurrentRevisionRole).toString());
        const auto target = regionId(selected, "stage2", "line", 6);
        const auto preview =
            previewChoice(*controller.store()->file("choices"), revision(), target.toStdString(), ChoiceAction::Ignore);
        CHECK(controller.store()->applyChoice("choices", preview) == Transition::Applied);
        CHECK(identity.count() == 0 && sources.count() == 0 && result.count() == 1 && decisions.count() == 1);
    });
    suite.run("accepted controller edit keeps source signals and row status in sync", [] {
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
        CHECK(controller.store()->appendFile(viewFile()));
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
        CHECK(controller.commands()->editResult("choices", originalRevision, 0, 0, QString::fromUtf8("é ")));
        CHECK(controller.selectedFile() == selected && selected->stage2() == originalSource);
        CHECK(sources.count() == 0 && identity.count() == 0 && result.count() == 1);
        CHECK(status.count() == 1 && rows.count() == 1);
        CHECK(selected->resultText() == QString::fromUtf8("é ") + originalText);
        CHECK(controller.store()->file("choices")->current().draft.text
              == selected->resultText().toUtf8().toStdString());
        CHECK(selected->currentRevision() == row.data(SessionModel::CurrentRevisionRole).toString());
        CHECK(selected->status() == row.data(SessionModel::StatusRole).toString());
        CHECK(selected->progress() == row.data(SessionModel::ProgressRole).toMap());
        CHECK(!controller.commands()->editResult("choices", originalRevision, 0, 0, "stale"));
        CHECK(sources.count() == 0 && identity.count() == 0 && result.count() == 1 && status.count() == 1
              && rows.count() == 1);
    });
    suite.run("selected file ranges, kinds and region states follow each new revision", [] {
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
        CHECK(controller.store()->appendFile(viewFile()));
        CHECK(controller.selection()->select("choices"));
        int changes = 0;
        QObject::connect(controller.selectedFile(), &SelectedFile::statusChanged, [&] { ++changes; });
        const auto revision = [&] { return controller.store()->file("choices")->current().id; };

        CHECK(controller.store()->editResult("choices", revision(), {{0, 0}, "inserted é\U0001F600\n"})
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
            CHECK(controller.store()->applyChoice("choices", preview, true) == Transition::Applied);
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
        CHECK(controller.store()->editResult("choices", revision(), {{at, at}, "hand "}) == Transition::Applied);
        file = controller.selectedFile();
        CHECK(file->currentRevision() == "5");
        const auto regions = file->regions();
        CHECK(regions[6].toMap().value("state").toString() == "manual"
              && regions[7].toMap().value("state").toString() == "manual");
        CHECK(regions[8].toMap().value("state").toString() == "undecided"
              && regions[9].toMap().value("state").toString() == "undecided");
        checkRange(regions[6].toMap(), "result", file->resultText(), 6, 7);
        CHECK(controller.store()->undo("choices", revision()) == Transition::Applied);
        file = controller.selectedFile();
        CHECK(file->regions()[6].toMap().value("state").toString() == "accepted");
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
            for (const auto &value : file->regions()) {
                const auto *anchor = view.controller.store()->file("choices")->correspondence()->anchor(
                    value.toMap().value("id").toString().toStdString());
                if (anchor && anchor->manualOverlap) ++overlapping;
            }
        }
        CHECK(overlapping > 0);
    });
    suite.run("deciding both sides of a marked section removes its marker lines from the shown result", [] {
        RegionView view(test_support::markedFile());
        auto file = view.file();
        const auto preview = [&](const char *side, ChoicePreviewFlow::Action action) {
            return view.controller.choices()->previewChoice("choices", file->currentRevision(),
                                                            regionId(file, side, "section", 2), action);
        };
        CHECK(preview("stage2", ChoicePreviewFlow::Action::Accept));
        CHECK(view.controller.feedback()->message()
              == "Accept changes only this scope and leaves the other side undecided.");
        CHECK(view.controller.choices()->applyChoice());
        file = view.file();
        CHECK(file->resultText() == QString::fromStdString(test_support::markedText()));
        CHECK(preview("stage3", ChoicePreviewFlow::Action::Ignore));
        CHECK(view.controller.feedback()->message().contains("marker lines and ignored text are removed"));
        CHECK(view.controller.choices()->applyChoice());
        file = view.file();
        const auto resolved = QStringLiteral("header\nleft first\nleft second\nbridge\n"
                                             "<<<<<<< ours\nleft later\n=======\nright later\n>>>>>>> theirs\ntail\n");
        CHECK(file->resultText() == resolved);
        CHECK(region(file, regionId(file, "stage2", "section", 2)).value("state").toString() == "accepted");
        CHECK(view.controller.store()->file("choices")->current().draft.unresolvedMarkers == 1);
        CHECK(view.undo()->resultText() == QString::fromStdString(test_support::markedText()));
        CHECK(view.redo()->resultText() == resolved);
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
    suite.run("typing across a region boundary marks it and a whole-file choice clears every hand edit", [] {
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
        auto &files = *view.controller.store();
        const auto preview = previewWholeFile(*files.file("choices"), view.revision(), SourceSide::Stage2);
        CHECK(files.applyWholeFile("choices", preview, true) == Transition::Applied);
        file = view.file();
        CHECK(handEdits(file) == noHandEdits);
        CHECK(!regionStates(file).contains("manual") && !regionStates(file).contains("undecided"));
    });
    suite.run("undo and redo restore each region's hand-edit flag with the text and decisions", [] {
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
        CHECK(region(file, line).value("handEdited").toBool() && !region(file, line).value("mapped").toBool());
        file = deleted.choose(regionId(file, "stage2", "section", 2), ChoiceAction::Accept);
        CHECK(deleted.text() == left && none(file) && region(file, line).value("state") == "accepted");
        file = deleted.undo();
        CHECK(region(file, line).value("handEdited").toBool() && region(file, line).value("state") == "undecided");
        file = deleted.redo();
        CHECK(none(file) && region(file, line).value("state") == "accepted");

        RegionView unchanged(test_support::choiceFile());
        auto &files = *unchanged.controller.store();
        const auto wholeLeft = [&] {
            return files.applyWholeFile(
                "choices", previewWholeFile(*files.file("choices"), unchanged.revision(), SourceSide::Stage2), true);
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
        test_support::TestSession model(directory.path() + "/recovery");
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
        const auto section =
            test_support::choiceTarget(*model.file("choices"), SourceSide::Stage2, ChoiceScope::Section)
                .source.contributionId;
        CHECK(model.applyChoice("choices",
                                previewChoice(*model.file("choices"), revision(), section, ChoiceAction::Accept), true)
              == Transition::Applied);
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
    suite.run("recovery replay restores each region's hand-edit flag with the text and decisions", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        test_support::TestSession model(directory.path() + "/recovery");
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
        const auto leftConflict =
            test_support::choiceTarget(*model.file("choices"), SourceSide::Stage2, ChoiceScope::Line, 2)
                .source.contributionId;
        CHECK(model.applyChoice("choices",
                                previewChoice(*model.file("choices"), revision(), leftConflict, ChoiceAction::Accept),
                                true)
              == Transition::Applied);
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
    suite.run("records must list their hand edits, and unknown anchors are rejected", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        test_support::TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(viewFile()));
        const auto at = model.file("choices")->current().draft.text.find("base conflict") + 5;
        CHECK(model.editResult("choices", 1, {{at, at}, "hand "}) == Transition::Applied);
        const auto recorded = model.file("choices")->correspondence()->handEditedAnchors();
        CHECK(recorded.size() == 4);
        auto replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
        CHECK(replay.succeeded && replay.files.size() == 1);
        CHECK(replay.files.front().correspondence()->handEditedAnchors() == recorded);
        // A record without the field is refused rather than read as having none.
        const auto journal = test_support::readFile(model.recoveryDirectory() + "/journal");
        test_support::rewriteLastRecoveryState(model.recoveryDirectory(), [&](QJsonObject &state) {
            auto current = state.value("current").toObject();
            CHECK(current.value("handEdited").toArray().size() == static_cast<qsizetype>(recorded.size()));
            CHECK(current.value("handEditedRanges").toArray().size() == 1);
            current.remove("handEdited");
            state.insert("current", current);
        });
        replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
        CHECK(!replay.succeeded && replay.error.contains("hand edits"));
        // Every recorded hand edit must name an anchor of the replayed draft.
        test_support::writeFile(model.recoveryDirectory() + "/journal", journal);
        test_support::rewriteLastRecoveryState(model.recoveryDirectory(), [](QJsonObject &state) {
            auto current = state.value("current").toObject();
            current.insert("handEdited", QJsonArray{state.value("path")});
            state.insert("current", current);
        });
        replay = replayRecovery(storage::RecoveryStore(model.recoveryDirectory()));
        CHECK(!replay.succeeded && replay.files.empty());
    });
    return suite.report();
}
