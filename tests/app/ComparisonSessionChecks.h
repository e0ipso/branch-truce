#pragma once

#include "ComparisonScene.h"

namespace comparison_scene {
// A supported choice file under another identity, so one session holds several.
inline FileSession namedChoiceFile(const std::string &id)
{
    auto sources = *test_support::choiceFile().snapshots();
    const auto path = id + ".txt";
    sources.path = path;
    const auto text = sources.launchResult.text.utf8;
    return FileSession(id, path, {text, {}, 0}, {.snapshots = std::move(sources), .initializeChoices = true});
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
    bool operator==(const WorkSnapshot &) const = default;
};

inline WorkSnapshot captureWork(Fixture &fixture, const SelectedFile &selected)
{
    const auto &current = fixture.file().current();
    const auto facts = fixture.file().facts();
    return {current.id,
            current.draft,
            fixture.file().canUndo(),
            fixture.file().canRedo(),
            QVariant(selected.progress()),
            facts.undecided,
            facts.unsaved,
            facts.resolved,
            facts.needsReview,
            facts.status,
            fixture.item("resultText")->property("text").toString(),
            fixture.item("stage2Text")->property("text").toString(),
            fixture.item("stage3Text")->property("text").toString()};
}

inline void checkUnchanged(Fixture &fixture, const SelectedFile &selected, const WorkSnapshot &before)
{
    CHECK(captureWork(fixture, selected) == before);
}

inline void expectDiagnostic(Fixture &fixture, const QString &diagnostic)
{
    auto *button = test_support::waitForStable(
        fixture.window, "Technical details",
        [&] {
            return test_support::reachableButton(fixture.window, fixture.window->contentItem(), "Technical details");
        },
        60);
    test_support::clickItem(fixture.window, button);
    settleFor("captured file diagnostic", [&] { return fixture.item("selectedProblemDiagnostic")->isVisible(); });
    auto *text = fixture.item("selectedProblemDiagnostic");
    CHECK(text->property("text").toString() == diagnostic);
    CHECK(text->property("readOnly").toBool() && text->property("selectByMouse").toBool());
    CHECK(text->property("textFormat").toInt() == 0); // TextEdit.PlainText
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    settleFor("file diagnostics closed",
              [&] { return !fixture.object("selectedProblemDetails")->property("visible").toBool(); });
}

inline void expectProgress(const Selection &selection, const SelectedFile &selected, bool saved, bool reviewed,
                           bool resolved, bool earlier, const QString &current, const QString &savedRevision,
                           const QString &reviewedRevision, const QString &stagedRevision, qulonglong undecided,
                           qulonglong conflicts = 0)
{
    CHECK(selection.fileId() == QStringLiteral("choices"));
    const auto progress = selected.progress();
    CHECK_EQ(progress.value(QStringLiteral("saved")).toBool(), saved);
    CHECK_EQ(progress.value(QStringLiteral("reviewed")).toBool(), reviewed);
    CHECK_EQ(progress.value(QStringLiteral("resolved")).toBool(), resolved);
    CHECK_EQ(progress.value(QStringLiteral("earlierResolved")).toBool(), earlier);
    CHECK_EQ(progress.value(QStringLiteral("currentRevision")).toString(), current);
    CHECK_EQ(progress.value(QStringLiteral("savedRevision")).toString(), savedRevision);
    CHECK_EQ(progress.value(QStringLiteral("reviewedRevision")).toString(), reviewedRevision);
    CHECK_EQ(progress.value(QStringLiteral("stagedRevision")).toString(), stagedRevision);
    CHECK_EQ(progress.value(QStringLiteral("undecided")).toULongLong(), undecided);
    CHECK_EQ(progress.value(QStringLiteral("conflicts")).toULongLong(), conflicts);
}

} // namespace comparison_scene
