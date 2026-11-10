// The session model: the rows, roles, statuses and handoff the window lists, and when it notifies.
#include "SessionFixtures.h"
#include "TestHarness.h"
#include "TestSession.h"
#include "app/SessionModel.h"
#include "app/Translations.h"
#include "fixtures/SyncFaults.h"
#include <QCoreApplication>
#include <QMap>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTemporaryDir>
#include <limits>

using namespace branch_truce;
using namespace session_fixtures;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    test_support::Suite suite("SessionModelTests");
    suite.run("empty model and invalid indexes", [] {
        TestSession model;
        CHECK(model.totalFiles() == 0 && model.rowCount() == 0 && model.resolvedFiles() == 0);
        CHECK(!model.canFinish() && model.finishBlockers().isEmpty());
        CHECK(model.handoff().value("outcome").toString() == "No files are loaded in this session.");
        CHECK(!model.file("missing") && !model.data({}, SessionModel::PathRole).isValid());
        CHECK(model.appendFile(FileSession("id", "file", draft())));
        CHECK(model.rowCount(model.index(0)) == 0 && !model.index(1).isValid());
        CHECK(!model.data(model.index(0), -1).isValid());
        TestSession other;
        CHECK(other.appendFile(FileSession("other", "path", draft())));
        CHECK(!model.data(other.index(0), SessionModel::PathRole).isValid());
    });
    suite.run("named roles preserve byte paths, Unicode and 64-bit decision order", [] {
        TestSession model;
        auto unicode = draft("A\xc3\xa9\xf0\x9f\x98\x80\n");
        unicode.decisions["left"].order = std::numeric_limits<std::uint64_t>::max();
        unicode.decisions["right"] = {DecisionValue::Ignored, {}};
        const std::string path = std::string(":literal\t\n") + static_cast<char>(0xff);
        CHECK(model.appendFile(FileSession("id", path, unicode)));
        CHECK(model.file("id")->path() == path);
        CHECK(model.file("id")->current().draft.text == unicode.text);
        CHECK(role(model, SessionModel::CurrentRevisionRole).toString() == "1");
        CHECK(model.file("id")->current().result == 1 && model.file("id")->current().decisions == 1);
        const auto decisions = role(model, SessionModel::DecisionsRole).toList();
        CHECK(decisions.size() == 2);
        CHECK(decisions[0].toMap().value("value").toString() == "accepted");
        CHECK(decisions[0].toMap().value("order").toString() == "18446744073709551615");
        CHECK(decisions[1].toMap().value("value").toString() == "ignored");
        CHECK(!decisions[1].toMap().contains("order"));
        const auto names = model.roleNames();
        // Progress and the view roles follow the recovery roles.
        CHECK(names.size() == SessionModel::ConflictCountRole - SessionModel::FileIdRole + 1);
        CHECK(names.value(SessionModel::ProgressRole) == "progress");
        CHECK(names.value(SessionModel::StatusIconRole) == "statusIcon");
        CHECK(names.value(SessionModel::StatusLineRole) == "statusLine");
        CHECK(names.value(SessionModel::ChipsRole) == "chips");
        CHECK(names.value(SessionModel::SummaryRole) == "summary");
        CHECK(names.value(SessionModel::ConflictCountRole) == "conflictCount");
        QSet<QByteArray> unique;
        for (auto it = names.cbegin(); it != names.cend(); ++it) {
            CHECK(!it.value().isEmpty() && !unique.contains(it.value()));
            unique.insert(it.value());
        }
    });
    suite.run("conflict counts follow marker history without erasing source decisions", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::splitFile()));
        const auto launch = model.file("choices")->current().draft;
        CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 1);
        CHECK(role(model, SessionModel::UndecidedRole).toInt() > 1);
        auto edited = launch;
        edited.text = "manual result without conflict markers\n";
        edited.unresolvedMarkers = 0;
        CHECK(model.replaceDraft("choices", 1, edited) == Transition::Applied);
        CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 0);
        CHECK(role(model, SessionModel::UndecidedRole).toInt() > 0);
        CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft == launch);
        CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 1);
        CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(model.file("choices")->current().draft == edited);
        CHECK(role(model, SessionModel::ConflictCountRole).toInt() == 0);
        CHECK(replayedFile(model.recoveryDirectory()).current().draft == edited);
    });
    suite.run("saved and reviewed drafts do not receive the published check tile", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(sessionFile("status")));
        const auto revision = model.file("status")->current().id;
        CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(model.recordCheckpoint("status", revision, Outcome::Succeeded) == Transition::Applied);
        CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(model.beginMarkResolved("status", revision) == Transition::Applied);
        CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(model.recordWrite("status", revision, Outcome::Succeeded) == Transition::Applied);
        CHECK(role(model, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(model.recordStage("status", revision, Outcome::Succeeded, "verified-object", "100644")
              == Transition::Applied);
        CHECK(role(model, SessionModel::StatusIconRole).toString() == "emblem-checked");
    });
    suite.run("row insertion and notifications occur only for applied commands", [] {
        TestSession model;
        int summaries = 0;
        int changes = 0;
        int insertions = 0;
        QObject::connect(&model.rows, &SessionModel::summaryChanged, &model, [&] { ++summaries; });
        QObject::connect(&model.rows, &QAbstractItemModel::rowsInserted, &model,
                         [&](const QModelIndex &parent, int first, int last) {
                             CHECK(!parent.isValid() && first == last);
                             ++insertions;
                         });
        QObject::connect(&model.rows, &QAbstractItemModel::dataChanged, &model,
                         [&](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &roles) {
                             CHECK(top.row() == 0 && top == bottom && roles.isEmpty());
                             ++changes;
                         });
        CHECK(model.appendFile(FileSession("id", "file", draft())));
        const QPersistentModelIndex first(model.index(0));
        CHECK(!model.appendFile(FileSession("id", "duplicate", draft())));
        CHECK(summaries == 1 && insertions == 1 && changes == 0);
        CHECK(model.replaceDraft("missing", 1, draft("missing")) == Transition::StaleRevision);
        CHECK(model.replaceDraft("id", 1, draft()) == Transition::NoChange);
        CHECK(model.replaceDraft("id", 99, draft("stale")) == Transition::StaleRevision);
        CHECK(model.recordWrite("id", 1, Outcome::Succeeded) == Transition::StaleRevision);
        CHECK(summaries == 1 && changes == 0);
        CHECK(model.replaceDraft("id", 1, draft("changed")) == Transition::Applied);
        CHECK(changes == 1 && summaries == 2);
        CHECK(model.beginMarkResolved("id", 2) == Transition::Applied);
        CHECK(model.recordWrite("id", 2, Outcome::Succeeded) == Transition::InvalidOutcome);
        CHECK(changes == 2 && summaries == 3);
        CHECK(model.appendFile(FileSession("second", "second", draft())));
        CHECK(first.isValid() && first.row() == 0 && first.data(SessionModel::FileIdRole) == "id");
        CHECK(insertions == 2 && summaries == 4);
    });
    suite.run("Save and publication roles stay distinct and handoff includes every file", [] {
        TestSession model;
        CHECK(model.appendFile(FileSession("first", "a", draft())));
        CHECK(model.recordCheckpoint("first", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(!role(model, SessionModel::UnsavedRole).toBool());
        CHECK(role(model, SessionModel::ReviewNeededRole).toBool());
        CHECK(!model.file("first")->publication().reviewed());
        CHECK(!model.file("first")->publication().written());
        CHECK(!model.file("first")->publication().staged());
        CHECK(model.file("first")->revision(*model.file("first")->publication().checkpoint())->draft.text
              == "result\n");
        resolve(model, "first");
        CHECK(model.canFinish() && model.resolvedFiles() == 1 && model.finishBlockers().isEmpty());
        CHECK(model.file("first")->publication().staged()->objectId == "verified-object");
        CHECK(model.file("first")->publication().staged()->mode == "100644");
        CHECK(model.appendFile(
            FileSession("second", "b", draft(), {.support = Support::OutsideApp, .supportDetail = "binary"})));
        CHECK(model.totalFiles() == 2 && model.resolvedFiles() == 1 && !model.canFinish());
        CHECK(role(model, SessionModel::SupportRole, 1).value<FileStates::SupportKind>()
              == FileStates::SupportKind::OutsideApp);
        CHECK(role(model, SessionModel::SupportDetailRole, 1).toString() == "binary");
        CHECK(model.finishBlockers().size() == 1);
        CHECK(model.finishBlockers()[0].toMap().value("fileId").toString() == "second");
        CHECK(model.setSupport("second", Support::Unreadable, "read failed") == Transition::Applied);
        CHECK(role(model, SessionModel::SupportRole, 1).value<FileStates::SupportKind>()
              == FileStates::SupportKind::Unreadable);
        CHECK(!model.canFinish());
        CHECK(model.setSupport("second", Support::Supported) == Transition::Applied);
        resolve(model, "second");
        CHECK(model.canFinish() && model.resolvedFiles() == 2);
        CHECK(model.recordCheckpoint("second", 1, Outcome::Failed, "disk full") == Transition::Applied);
        CHECK(!model.canFinish() && model.resolvedFiles() == 1);
        CHECK(model.file("second")->publication().checkpointOutcome()->detail == "disk full");
        CHECK(role(model, SessionModel::StatusRole, 1).toString() == "Draft save failed");
    });
    suite.run("the English catalog supplies the plural forms of numerus messages", [] {
        const auto left = [](int count) {
            return QCoreApplication::translate("FilePresentation", "%n conflict(s) left", nullptr, count);
        };
        CHECK(left(1) == "1 conflict left" && left(2) == "2 conflicts left");
        CHECK(QCoreApplication::translate("SessionFooter", "%1 of %n file(s) resolved", nullptr, 1).arg(0)
              == "0 of 1 file resolved");
    });
    suite.run("handoff counts use singular and plural file wording", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(sessionFile("first")));
        CHECK(model.handoff().value("outcome").toString().startsWith("1 of 1 file in this session remains"));
        resolve(model, "first");
        CHECK(model.handoff().value("outcome").toString().startsWith("1 file in this session is written"));
        CHECK(model.appendFile(sessionFile("second")));
        CHECK(model.handoff().value("outcome").toString().startsWith("1 of 2 files in this session remain"));
        resolve(model, "second");
        CHECK(model.handoff().value("outcome").toString().startsWith("All 2 files in this session are written"));
    });
    suite.run("handoff puts each mixed outcome in one category and pending input stays failed until retried", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        TestSession model(directory.path() + "/recovery", faults);
        auto choices = draft();
        choices.decisions["left"] = {};
        CHECK(model.appendFile(sessionFile("undecided", choices)));
        CHECK(model.appendFile(sessionFile("saved")));
        CHECK(model.recordCheckpoint("saved", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.appendFile(sessionFile("review")));
        CHECK(model.appendFile(sessionFile("newer")));
        resolve(model, "newer");
        CHECK(model.replaceDraft("newer", 1, draft("newer draft\n")) == Transition::Applied);
        CHECK(model.appendFile(sessionFile("written")));
        CHECK(model.beginMarkResolved("written", 1) == Transition::Applied);
        CHECK(model.recordCheckpoint("written", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.recordWrite("written", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.appendFile(sessionFile("published")));
        resolve(model, "published");
        CHECK(model.appendFile(sessionFile("failed")));
        CHECK(model.beginMarkResolved("failed", 1) == Transition::Applied);
        CHECK(model.recordCheckpoint("failed", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.recordWrite("failed", 1, Outcome::Failed, "disk full") == Transition::Applied);
        CHECK(model.appendFile(FileSession("outside", "diagram.png", {},
                                           {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(model.appendFile(sessionFile("pending")));
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(model.replaceDraft("pending", 1, draft("pending input\n")) == Transition::Blocked);
            CHECK(faults->failures == 1);
        }
        CHECK(model.hasPendingRecovery() && !model.canFinish());

        struct Expected {
            const char *category;
            FileStates::NextAction action;
        };
        using enum FileStates::NextAction;
        const QMap<QString, Expected> expected{
            {"undecided", {"undecided", Decide}},          {"saved", {"saved-draft", MarkResolved}},
            {"review", {"review-needed", MarkResolved}},   {"newer", {"newer-draft", ApplyOrDiscard}},
            {"written", {"written-unresolved", CheckGit}}, {"published", {"published", None}},
            {"failed", {"failed", MarkResolved}},          {"outside", {"outside-app", ExplainOutsideApp}},
            {"pending", {"failed", RetryRecovery}}};
        auto summary = model.handoff();
        auto entries = handoffEntries(summary);
        CHECK(entries.size() == expected.size());
        for (auto wanted = expected.cbegin(); wanted != expected.cend(); ++wanted) {
            const auto entry = entries.value(wanted.key());
            if (entry.value("category").toString() != wanted->category
                || entry.value("action").value<FileStates::NextAction>() != wanted->action)
                throw std::runtime_error((wanted.key() + ": " + entry.value("category").toString() + "/"
                                          + QString::number(entry.value("action").toInt()))
                                             .toStdString());
            CHECK(entry.value("outstanding").toBool() == (wanted.key() != "published"));
            CHECK(entry.value("outstanding").toBool() != entry.value("blockers").toStringList().isEmpty());
            CHECK(entry.value("outstanding").toBool() != entry.value("actionLabel").toString().isEmpty());
        }
        QStringList order;
        for (const auto &group : summary.value("groups").toList()) order << group.toMap().value("category").toString();
        CHECK(order
              == QStringList({"undecided", "saved-draft", "review-needed", "newer-draft", "written-unresolved",
                              "failed", "outside-app", "published"}));
        const auto counts = summary.value("counts").toMap();
        CHECK(counts.value("failed").toInt() == 2 && counts.value("review-needed").toInt() == 1);
        CHECK(counts.value("outside-app").toInt() == 1 && counts.value("published").toInt() == 1);
        CHECK(!summary.value("clean").toBool());
        CHECK(summary.value("totalFiles").toInt() == 9 && summary.value("outstandingFiles").toInt() == 8);
        CHECK(summary.value("outcome").toString()
              == "8 of 9 files in this session remain outstanding. This session is not finished.");
        CHECK(entries.value("pending").value("status").toString() == "Recovery failed; pending input retained");
        CHECK(entries.value("pending").value("blockers").toStringList().contains("Pending input needs recovery retry"));
        CHECK(entries.value("pending").value("actionLabel").toString() == "Retry recovery");
        CHECK(entries.value("pending").value("unsaved").toBool());
        CHECK(entries.value("review").value("actionLabel").toString() == "Save or Mark resolved");
        CHECK(entries.value("undecided").value("undecided").toULongLong() == 1);
        CHECK(entries.value("failed").value("status").toString() == "Repository write failed");
        CHECK(entries.value("newer").value("status").toString() == "Earlier result staged; current draft not applied");

        CHECK(model.retryRecovery("pending") == Transition::Applied);
        CHECK(!model.hasPendingRecovery() && model.file("pending")->current().draft == draft("pending input\n"));
        summary = model.handoff();
        entries = handoffEntries(summary);
        CHECK(entries.value("pending").value("category").toString() == "review-needed");
        CHECK(entries.value("pending").value("action").value<FileStates::NextAction>()
              == FileStates::NextAction::MarkResolved);
        CHECK(entries.value("pending").value("status").toString() == "Current result needs review");
        CHECK(
            !entries.value("pending").value("blockers").toStringList().contains("Pending input needs recovery retry"));
        CHECK(summary.value("counts").toMap().value("failed").toInt() == 1);
        CHECK(summary.value("counts").toMap().value("review-needed").toInt() == 2);
        CHECK(summary.value("outstandingFiles").toInt() == 8 && !summary.value("clean").toBool());
    });
    suite.run("a failed write, stage or recovery shows the handoff's icon in the list", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        for (const auto *id : {"write", "stage", "recovery"}) CHECK(model.appendFile(sessionFile(id)));
        const auto icon = [&](const std::string &id) {
            return role(model, SessionModel::StatusIconRole, model.rowOf(id)).toString();
        };
        // A fresh draft needs review and Save; the cached facts must follow each outcome.
        CHECK(icon("write") == "vcs-locally-modified");
        CHECK(model.beginMarkResolved("write", 1) == Transition::Applied);
        CHECK(model.recordCheckpoint("write", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(icon("write") == "document-save");
        CHECK(model.recordWrite("write", 1, Outcome::Failed, "disk full") == Transition::Applied);
        CHECK(model.beginMarkResolved("stage", 1) == Transition::Applied);
        CHECK(model.recordCheckpoint("stage", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.recordWrite("stage", 1, Outcome::Succeeded) == Transition::Applied);
        CHECK(model.recordStage("stage", 1, Outcome::Failed, {}, {}, "index locked") == Transition::Applied);
        model.recordRecoveryFailure("recovery", "store unavailable");
        const auto entries = handoffEntries(model.handoff());
        for (const auto *id : {"write", "stage", "recovery"}) {
            const auto entry = entries.value(id);
            CHECK(entry.value("category").toString() == "failed");
            CHECK(entry.value("icon").toString() == "dialog-error");
            CHECK(icon(id) == "dialog-error");
            CHECK(role(model, SessionModel::StatusRole, model.rowOf(id)) == entry.value("status"));
        }
        CHECK(role(model, SessionModel::StatusRole, model.rowOf("write")).toString() == "Repository write failed");
        CHECK(role(model, SessionModel::StatusRole, model.rowOf("recovery")).toString()
              == "Recovery requires attention; work remains outstanding");
    });
    suite.run("rows carry one status line, toned chips, a summary and the escaped path", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        auto choices = draft();
        choices.decisions["left"] = {};
        CHECK(model.appendFile(sessionFile("undecided", choices)));
        CHECK(model.appendFile(sessionFile("published")));
        resolve(model, "published");
        CHECK(model.appendFile(FileSession("outside", "diagram.png", {},
                                           {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(model.appendFile(FileSession("unreadable", "broken.txt", {}, {.support = Support::Unreadable})));
        const std::string bytes = std::string(":literal\t") + static_cast<char>(0xff);
        CHECK(model.appendFile(FileSession("bytes", bytes, draft())));
        const std::vector<std::string> displayOrder{"undecided", "bytes", "published", "outside", "unreadable"};
        for (int row = 0; row < static_cast<int>(displayOrder.size()); ++row) {
            const auto &expected = displayOrder[static_cast<std::size_t>(row)];
            CHECK(model.rowOf(expected) == row);
            CHECK(model.fileAt(row)->id() == expected);
            CHECK(role(model, SessionModel::FileIdRole, row).toString().toStdString() == expected);
        }
        CHECK(model.rowOf("missing") == -1);
        CHECK(model.supportCount(Support::Supported) == 3 && model.supportCount(Support::OutsideApp) == 1
              && model.supportCount(Support::Unreadable) == 1);
        const auto at = [&](const std::string &id, int value) { return role(model, value, model.rowOf(id)); };

        CHECK(at("undecided", SessionModel::StatusLineRole).toString() == "1 undecided");
        CHECK(at("published", SessionModel::StatusLineRole).toString() == "Resolved");
        CHECK(at("outside", SessionModel::StatusLineRole).toString() == "Handle outside the app");
        CHECK(at("unreadable", SessionModel::StatusLineRole).toString() == "Load error");
        // Views branch on the kind, never on the translated status text.
        CHECK(at("published", SessionModel::StatusKindRole).value<FileStates::StatusKind>()
              == FileStates::StatusKind::Resolved);
        CHECK(at("unreadable", SessionModel::StatusKindRole).value<FileStates::StatusKind>()
              == FileStates::StatusKind::Unreadable);
        CHECK(at("undecided", SessionModel::StatusIconRole).toString() == "vcs-conflicting");
        CHECK(at("published", SessionModel::StatusIconRole).toString() == "emblem-checked");
        CHECK(at("outside", SessionModel::StatusIconRole).toString() == "unknown");
        CHECK(at("unreadable", SessionModel::StatusIconRole).toString() == "dialog-error");

        CHECK(at("published", SessionModel::SummaryRole).toString()
              == "Written and resolved. Undecided: 0. Saved. Reviewed. Resolved.");
        CHECK(at("undecided", SessionModel::SummaryRole).toString()
              == "Source choices remain undecided. Undecided: 1. Not saved. Not reviewed. Not resolved.");
        const QSet<QString> tones{"neutral", "positive", "negative", "warning"};
        for (int row = 0; row < model.rowCount(); ++row) {
            CHECK(!role(model, SessionModel::SummaryRole, row).toString().contains(QStringLiteral(" \u00b7 ")));
            const auto chips = role(model, SessionModel::ChipsRole, row).toList();
            CHECK(chips.size() >= 5);
            // The first chip is the status sentence, without an icon.
            CHECK(chips.first().toMap().value("text") == role(model, SessionModel::StatusRole, row));
            CHECK(chips.first().toMap().value("icon").toString().isEmpty());
            for (const auto &chip : chips) {
                const auto fields = chip.toMap();
                CHECK(fields.size() == 3 && !fields.value("text").toString().isEmpty());
                CHECK(tones.contains(fields.value("tone").toString()));
            }
        }
        const auto texts = [&](const std::string &id) {
            QStringList result;
            for (const auto &chip : at(id, SessionModel::ChipsRole).toList())
                result << chip.toMap().value("text").toString();
            return result;
        };
        CHECK(texts("published")
              == QStringList({"Written and resolved", "Manual Save checkpoint retained", "Undecided: 0", "Saved",
                              "Reviewed", "Resolved"}));
        CHECK(texts("undecided")
              == QStringList({"Source choices remain undecided", "Unsaved draft", "Undecided: 1", "Not saved",
                              "Not reviewed", "Not resolved"}));
        // A file the app cannot edit has no draft chip.
        CHECK(!texts("outside").join("\n").contains("draft", Qt::CaseInsensitive));
        const auto undecidedChip = at("undecided", SessionModel::ChipsRole).toList().value(2).toMap();
        CHECK(undecidedChip.value("icon").toString() == "dialog-question"
              && undecidedChip.value("tone").toString() == "negative");

        // One escaper for every surface: the path role, the handoff and the finish blockers.
        const auto escaped = QStringLiteral(":literal\\t\\xff");
        CHECK(at("bytes", SessionModel::PathRole).toString() == escaped);
        CHECK(at("bytes", Qt::DisplayRole).toString() == escaped);
        CHECK(model.displayPath("bytes") == escaped && model.displayPath("missing").isEmpty());
        CHECK(model.file("bytes")->path() == bytes);
        CHECK(handoffEntries(model.handoff()).value("bytes").value("path").toString() == escaped);
        bool blocked = false;
        for (const auto &entry : model.finishBlockers()) {
            if (entry.toMap().value("fileId").toString() != "bytes") continue;
            CHECK(entry.toMap().value("path").toString() == escaped);
            blocked = true;
        }
        CHECK(blocked);
    });
    suite.run("Qt history restores atomic decisions, invalidates review and fences discard callbacks", [] {
        TestSession model;
        CHECK(model.appendFile(FileSession("id", "file", draft())));
        resolve(model, "id");
        auto changed = draft("new result");
        changed.decisions["left"] = {DecisionValue::Ignored, {}};
        CHECK(model.replaceDraft("id", 1, changed) == Transition::Applied);
        CHECK(role(model, SessionModel::CanUndoRole).toBool());
        CHECK(model.file("id")->facts().earlierStaged && !model.canFinish());
        CHECK(model.undo("id", 2) == Transition::Applied);
        CHECK(model.file("id")->current().draft.text == "result\n");
        CHECK(role(model, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "accepted");
        CHECK(role(model, SessionModel::CanRedoRole).toBool());
        CHECK(role(model, SessionModel::ReviewNeededRole).toBool() && !model.canFinish());
        CHECK(model.redo("id", 3) == Transition::Applied);
        CHECK(model.file("id")->current().draft.text == "new result");
        CHECK(role(model, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "ignored");
        CHECK(model.discardToCheckpoint("id", 4) == Transition::Applied);
        CHECK(model.recordCheckpoint("id", 2, Outcome::Succeeded) == Transition::StaleRevision);
        CHECK(*model.file("id")->publication().checkpoint() == 1);
        CHECK(model.discardToStaged("id", 5) == Transition::Applied);
        CHECK(model.canFinish() && role(model, SessionModel::CurrentRevisionRole).toString() == "1");
        CHECK(model.recordCheckpoint("id", 4, Outcome::Failed) == Transition::StaleRevision);
        CHECK(model.canFinish());
    });
    suite.run("a settled file reads ready to mark resolved until undo reopens a choice", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        TestSession model(directory.path() + "/recovery");
        CHECK(model.appendFile(test_support::markedFile()));
        const auto row = [&] { return model.rowOf("choices"); };
        const auto choose = [&](SourceSide side, std::size_t section) {
            const auto &file = *model.file("choices");
            const auto &target = test_support::choiceTarget(file, side, ChoiceScope::Section, section);
            return model.applyChoice(
                "choices",
                previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::ResolveUsingSide),
                true);
        };
        const auto settled = [&] {
            return role(model, SessionModel::ProgressRole, row()).toMap().value("settled").toBool();
        };
        const auto line = [&] { return role(model, SessionModel::StatusLineRole, row()).toString(); };
        CHECK(!settled() && line() != "Ready to mark resolved");
        CHECK(choose(SourceSide::Stage2, 0) == Transition::Applied);
        CHECK(!settled());
        CHECK(choose(SourceSide::Stage3, 1) == Transition::Applied);
        CHECK(settled() && line() == "Ready to mark resolved");
        // Ready is not reviewed or resolved, and the handoff keeps its existing category.
        CHECK(role(model, SessionModel::StatusRole, row()).toString() == "Current result needs review");
        CHECK(!role(model, SessionModel::ResolvedRole, row()).toBool());
        CHECK(model.undo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(!settled() && line() != "Ready to mark resolved");
        CHECK(model.redo("choices", model.file("choices")->current().id) == Transition::Applied);
        CHECK(settled());
        model.recordRecoveryFailure("choices", "store unavailable");
        CHECK(!settled() && line() != "Ready to mark resolved");
    });
    suite.run("unconfirmed choices and marker blockers are visible without mutation", [] {
        TestSession model;
        auto undecided = draft();
        undecided.decisions["left"] = {};
        CHECK(model.appendFile(FileSession("id", "file", undecided)));
        int changes = 0;
        QObject::connect(&model.rows, &SessionModel::summaryChanged, &model, [&] { ++changes; });
        CHECK(model.beginMarkResolved("id", 1) == Transition::ConfirmationRequired);
        CHECK(changes == 0 && !model.file("id")->publication().reviewed());
        CHECK(role(model, SessionModel::UndecidedRole).toULongLong() == 1);
        CHECK(model.beginMarkResolved("id", 1, true) == Transition::Applied);
        CHECK(model.file("id")->publication().reviewed()->confirmedUndecided);
        undecided.text = "=======\n";
        CHECK(model.replaceDraft("id", 1, undecided) == Transition::Applied);
        CHECK(model.beginMarkResolved("id", 2, true) == Transition::Blocked);
        CHECK(changes == 2);
        CHECK(role(model, SessionModel::FinishBlockersRole).toStringList().contains("Known conflict markers remain"));
    });
    for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain}) {
        suite.run("a delayed write stays visible after a newer Save: " + nameOf(outcome), [=] {
            TestSession model;
            CHECK(model.appendFile(FileSession("id", "file", draft())));
            CHECK(model.beginMarkResolved("id", 1) == Transition::Applied);
            CHECK(model.recordCheckpoint("id", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(model.replaceDraft("id", 1, draft("new saved draft")) == Transition::Applied);
            CHECK(model.recordCheckpoint("id", 2, Outcome::Succeeded) == Transition::Applied);
            int changes = 0;
            QObject::connect(&model.rows, &SessionModel::summaryChanged, &model, [&] { ++changes; });
            CHECK(model.recordWrite("id", 1, outcome, "delayed diagnostic") == Transition::Applied);
            CHECK(changes == 1);
            const auto &result = *model.file("id")->publication().writeOutcome();
            const auto status = outcome == Outcome::Succeeded ? "Written, unresolved"
                : outcome == Outcome::Failed                  ? "Repository write failed"
                                                              : "Outcome uncertain; verification required";
            CHECK(result.revision == 1);
            CHECK(result.outcome == outcome && result.detail == "delayed diagnostic");
            CHECK(role(model, SessionModel::StatusRole).toString() == status);
            CHECK(role(model, SessionModel::CurrentRevisionRole).toString() == "2");
            CHECK(*model.file("id")->publication().checkpoint() == 2);
            CHECK(model.file("id")->publication().reviewed()->revision == 1);
            CHECK(!role(model, SessionModel::UnsavedRole).toBool()
                  && role(model, SessionModel::ReviewNeededRole).toBool());
            CHECK(!model.canFinish() && model.resolvedFiles() == 0 && !model.finishBlockers().isEmpty());
            const auto &written = model.file("id")->publication().written();
            CHECK(outcome == Outcome::Succeeded ? written == RevisionId{1} : !written);
        });
    }
    return suite.report();
}
