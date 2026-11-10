// The session model: the rows, roles, statuses and handoff the window lists, and when it notifies.
#include "SessionFixtures.h"
#include "TestHarness.h"
#include "TestSession.h"
#include "app/SessionModel.h"
#include "app/Translations.h"
#include "fixtures/Await.h"
#include "fixtures/EventLoop.h"
#include "fixtures/SyncFaults.h"
#include "session/BackgroundAlignment.h"
#include <QCoreApplication>
#include <QMap>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTemporaryDir>
#include <limits>

using test_support::await;

using namespace branch_truce;
using namespace session_fixtures;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(app);
    test_support::Suite suite("SessionModelTests");
    suite.run("empty model and invalid indexes", [] {
        auto [store, rows] = test_support::TestSession{};
        CHECK(rows.totalFiles() == 0 && rows.rowCount() == 0 && rows.resolvedFiles() == 0);
        CHECK(!SessionHandoff::build(store, {}).clean && finishBlockers(store).isEmpty());
        CHECK(SessionHandoff::build(store, {}).outcome == "No files are loaded in this session.");
        CHECK(!store.file("missing") && !rows.data({}, SessionModel::PathRole).isValid());
        CHECK(store.appendFile(FileSession("id", "file", draft())));
        test_support::awaitRecovery(store);
        CHECK(rows.rowCount(rows.index(0)) == 0 && !rows.index(1).isValid());
        CHECK(!rows.data(rows.index(0), -1).isValid());
        auto [otherStore, otherRows] = test_support::TestSession{};
        CHECK(otherStore.appendFile(FileSession("other", "path", draft())));
        test_support::awaitRecovery(otherStore);
        CHECK(!rows.data(otherRows.index(0), SessionModel::PathRole).isValid());
    });
    suite.run("named roles preserve byte paths, Unicode and 64-bit decision order", [] {
        auto [store, rows] = test_support::TestSession{};
        auto unicode = draft("A\xc3\xa9\xf0\x9f\x98\x80\n");
        unicode.decisions["left"].order = std::numeric_limits<std::uint64_t>::max();
        unicode.decisions["right"] = {DecisionValue::Ignored, {}};
        const std::string path = std::string(":literal\t\n") + static_cast<char>(0xff);
        CHECK(store.appendFile(FileSession("id", path, unicode)));
        test_support::awaitRecovery(store);
        CHECK(store.file("id")->path() == path);
        CHECK(store.file("id")->current().draft.text == unicode.text);
        CHECK(role(rows, SessionModel::CurrentRevisionRole).toString() == "1");
        CHECK(store.file("id")->current().result == 1 && store.file("id")->current().decisions == 1);
        const auto decisions = role(rows, SessionModel::DecisionsRole).toList();
        CHECK(decisions.size() == 2);
        CHECK(decisions[0].toMap().value("value").toString() == "accepted");
        CHECK(decisions[0].toMap().value("order").toString() == "18446744073709551615");
        CHECK(decisions[1].toMap().value("value").toString() == "ignored");
        CHECK(!decisions[1].toMap().contains("order"));
        const auto names = rows.roleNames();
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
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::splitFile()));
        test_support::awaitRecovery(store);
        const auto launch = store.file("choices")->current().draft;
        CHECK(role(rows, SessionModel::ConflictCountRole).toInt() == 1);
        CHECK(role(rows, SessionModel::UndecidedRole).toInt() > 1);
        auto edited = launch;
        edited.text = "manual result without conflict markers\n";
        edited.unresolvedMarkers = 0;
        CHECK(await(store.replaceDraft("choices", 1, edited)) == Transition::Applied);
        CHECK(role(rows, SessionModel::ConflictCountRole).toInt() == 0);
        CHECK(role(rows, SessionModel::UndecidedRole).toInt() > 0);
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft == launch);
        CHECK(role(rows, SessionModel::ConflictCountRole).toInt() == 1);
        CHECK(await(store.redo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(store.file("choices")->current().draft == edited);
        CHECK(role(rows, SessionModel::ConflictCountRole).toInt() == 0);
        CHECK(replayedFile(store.recoveryDirectory()).current().draft == edited);
    });
    suite.run("saved and reviewed drafts do not receive the published check tile", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        CHECK(store.appendFile(sessionFile("status")));
        test_support::awaitRecovery(store);
        const auto revision = store.file("status")->current().id;
        CHECK(role(rows, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(await(store.recordCheckpoint("status", revision, Outcome::Succeeded)) == Transition::Applied);
        CHECK(role(rows, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(await(store.beginMarkResolved("status", revision)) == Transition::Applied);
        CHECK(role(rows, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(await(store.recordWrite("status", revision, Outcome::Succeeded)) == Transition::Applied);
        CHECK(role(rows, SessionModel::StatusIconRole).toString() != "emblem-checked");
        CHECK(await(store.recordStage("status", revision, Outcome::Succeeded, "verified-object", "100644"))
              == Transition::Applied);
        CHECK(role(rows, SessionModel::StatusIconRole).toString() == "emblem-checked");
    });
    suite.run("row insertion and notifications occur only for applied commands", [] {
        auto [store, rows] = test_support::TestSession{};
        int summaries = 0;
        int changes = 0;
        int insertions = 0;
        QObject::connect(&rows, &SessionModel::summaryChanged, &store, [&] { ++summaries; });
        QObject::connect(&rows, &QAbstractItemModel::rowsInserted, &store,
                         [&](const QModelIndex &parent, int first, int last) {
                             EXPECT(!parent.isValid() && first == last);
                             ++insertions;
                         });
        QObject::connect(&rows, &QAbstractItemModel::dataChanged, &store,
                         [&](const QModelIndex &top, const QModelIndex &bottom, const QList<int> &roles) {
                             EXPECT((top.row() == 0 || top.data(SessionModel::FileIdRole) == "second") && top == bottom
                                    && roles.isEmpty());
                             ++changes;
                         });
        CHECK(store.appendFile(FileSession("id", "file", draft())));
        test_support::awaitRecovery(store);
        const QPersistentModelIndex first(rows.index(0));
        CHECK(!store.appendFile(FileSession("id", "duplicate", draft())));
        CHECK(summaries == 2 && insertions == 1 && changes == 1);
        CHECK(await(store.replaceDraft("missing", 1, draft("missing"))) == Transition::StaleRevision);
        CHECK(await(store.replaceDraft("id", 1, draft())) == Transition::NoChange);
        CHECK(await(store.replaceDraft("id", 99, draft("stale"))) == Transition::StaleRevision);
        CHECK(await(store.recordWrite("id", 1, Outcome::Succeeded)) == Transition::StaleRevision);
        CHECK(summaries == 2 && changes == 1);
        CHECK(await(store.replaceDraft("id", 1, draft("changed"))) == Transition::Applied);
        CHECK(changes == 3 && summaries == 4);
        CHECK(await(store.beginMarkResolved("id", 2)) == Transition::Applied);
        CHECK(await(store.recordWrite("id", 2, Outcome::Succeeded)) == Transition::InvalidOutcome);
        CHECK(changes == 5 && summaries == 6);
        CHECK(store.appendFile(FileSession("second", "second", draft())));
        test_support::awaitRecovery(store);
        CHECK(first.isValid() && first.row() == 0 && first.data(SessionModel::FileIdRole) == "id");
        CHECK(insertions == 2 && summaries == 8);
    });
    suite.run("Save and publication roles stay distinct and handoff includes every file", [] {
        auto [store, rows] = test_support::TestSession{};
        CHECK(store.appendFile(FileSession("first", "a", draft())));
        CHECK(await(store.recordCheckpoint("first", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(!role(rows, SessionModel::UnsavedRole).toBool());
        CHECK(role(rows, SessionModel::ReviewNeededRole).toBool());
        CHECK(!store.file("first")->publication().reviewed());
        CHECK(!store.file("first")->publication().written());
        CHECK(!store.file("first")->publication().staged());
        CHECK(store.file("first")->revision(*store.file("first")->publication().checkpoint())->draft.text
              == "result\n");
        resolve(store, "first");
        CHECK(SessionHandoff::build(store, {}).clean && rows.resolvedFiles() == 1 && finishBlockers(store).isEmpty());
        CHECK(store.file("first")->publication().staged()->objectId == "verified-object");
        CHECK(store.file("first")->publication().staged()->mode == "100644");
        CHECK(store.appendFile(
            FileSession("second", "b", draft(), {.support = Support::OutsideApp, .supportDetail = "binary"})));
        test_support::awaitRecovery(store);
        CHECK(rows.totalFiles() == 2 && rows.resolvedFiles() == 1 && !SessionHandoff::build(store, {}).clean);
        CHECK(role(rows, SessionModel::SupportRole, 1).value<FileStates::SupportKind>()
              == FileStates::SupportKind::OutsideApp);
        CHECK(role(rows, SessionModel::SupportDetailRole, 1).toString() == "binary");
        CHECK(finishBlockers(store).size() == 1);
        CHECK(finishBlockers(store).contains("second"));
        CHECK(store.appendFile(FileSession("third", "c", draft())));
        test_support::awaitRecovery(store);
        resolve(store, "third");
        CHECK(!SessionHandoff::build(store, {}).clean && rows.resolvedFiles() == 2);
        CHECK(await(store.recordCheckpoint("third", 1, Outcome::Failed, "disk full")) == Transition::Applied);
        CHECK(!SessionHandoff::build(store, {}).clean && rows.resolvedFiles() == 1);
        CHECK(store.file("third")->publication().checkpointOutcome()->detail == "disk full");
        CHECK(role(rows, SessionModel::StatusRole, 2).toString() == "Draft save failed");
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
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(sessionFile("first")));
        test_support::awaitRecovery(store);
        CHECK(SessionHandoff::build(store, {}).outcome.startsWith("1 of 1 file in this session remains"));
        resolve(store, "first");
        CHECK(SessionHandoff::build(store, {}).outcome.startsWith("1 file in this session is written"));
        CHECK(store.appendFile(sessionFile("second")));
        test_support::awaitRecovery(store);
        CHECK(SessionHandoff::build(store, {}).outcome.startsWith("1 of 2 files in this session remain"));
        resolve(store, "second");
        CHECK(SessionHandoff::build(store, {}).outcome.startsWith("All 2 files in this session are written"));
    });
    suite.run("handoff puts each mixed outcome in one category and pending input stays failed until retried", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        const auto faults = std::make_shared<test_support::SyncFaults>();
        SessionStore store(directory.path() + "/recovery", faults);
        auto choices = draft();
        choices.decisions["left"] = {};
        CHECK(store.appendFile(sessionFile("undecided", choices)));
        CHECK(store.appendFile(sessionFile("saved")));
        CHECK(await(store.recordCheckpoint("saved", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(store.appendFile(sessionFile("review")));
        CHECK(store.appendFile(sessionFile("newer")));
        test_support::awaitRecovery(store);
        resolve(store, "newer");
        CHECK(await(store.replaceDraft("newer", 1, draft("newer draft\n"))) == Transition::Applied);
        CHECK(store.appendFile(sessionFile("written")));
        CHECK(await(store.beginMarkResolved("written", 1)) == Transition::Applied);
        CHECK(await(store.recordCheckpoint("written", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(store.recordWrite("written", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(store.appendFile(sessionFile("published")));
        test_support::awaitRecovery(store);
        resolve(store, "published");
        CHECK(store.appendFile(sessionFile("failed")));
        CHECK(await(store.beginMarkResolved("failed", 1)) == Transition::Applied);
        CHECK(await(store.recordCheckpoint("failed", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(store.recordWrite("failed", 1, Outcome::Failed, "disk full")) == Transition::Applied);
        CHECK(store.appendFile(FileSession("outside", "diagram.png", {},
                                           {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(store.appendFile(sessionFile("pending")));
        test_support::awaitRecovery(store);
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(await(store.replaceDraft("pending", 1, draft("pending input\n"))) == Transition::Blocked);
            CHECK(faults->failures == 1);
        }
        CHECK(store.hasPendingRecovery() && !SessionHandoff::build(store, {}).clean);

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
        auto summary = SessionHandoff::build(store, {}).toVariant();
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

        CHECK(await(store.retryRecovery("pending")) == Transition::Applied);
        CHECK(!store.hasPendingRecovery() && store.file("pending")->current().draft == draft("pending input\n"));
        summary = SessionHandoff::build(store, {}).toVariant();
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
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        for (const auto *id : {"write", "stage", "recovery"}) CHECK(store.appendFile(sessionFile(id)));
        test_support::awaitRecovery(store);
        const auto icon = [&](const std::string &id) {
            return role(rows, SessionModel::StatusIconRole, rows.rowOf(id)).toString();
        };
        // A fresh draft needs review and Save; the cached facts must follow each outcome.
        CHECK(icon("write") == "vcs-locally-modified");
        CHECK(await(store.beginMarkResolved("write", 1)) == Transition::Applied);
        CHECK(await(store.recordCheckpoint("write", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(icon("write") == "document-save");
        CHECK(await(store.recordWrite("write", 1, Outcome::Failed, "disk full")) == Transition::Applied);
        CHECK(await(store.beginMarkResolved("stage", 1)) == Transition::Applied);
        CHECK(await(store.recordCheckpoint("stage", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(store.recordWrite("stage", 1, Outcome::Succeeded)) == Transition::Applied);
        CHECK(await(store.recordStage("stage", 1, Outcome::Failed, {}, {}, "index locked")) == Transition::Applied);
        store.recordRecoveryFailure("recovery", ProblemCode::RecoveryStoreUnavailable);
        const auto entries = handoffEntries(SessionHandoff::build(store, {}).toVariant());
        for (const auto *id : {"write", "stage", "recovery"}) {
            const auto entry = entries.value(id);
            CHECK(entry.value("category").toString() == "failed");
            CHECK(entry.value("icon").toString() == "dialog-error");
            CHECK(icon(id) == "dialog-error");
            CHECK(role(rows, SessionModel::StatusRole, rows.rowOf(id)) == entry.value("status"));
        }
        CHECK(role(rows, SessionModel::StatusRole, rows.rowOf("write")).toString() == "Repository write failed");
        CHECK(role(rows, SessionModel::StatusRole, rows.rowOf("recovery")).toString()
              == "Recovery requires attention; work remains outstanding");
    });
    suite.run("rows carry one status line, toned chips, a summary and the escaped path", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        auto choices = draft();
        choices.decisions["left"] = {};
        CHECK(store.appendFile(sessionFile("undecided", choices)));
        CHECK(store.appendFile(sessionFile("published")));
        test_support::awaitRecovery(store);
        resolve(store, "published");
        CHECK(store.appendFile(FileSession("outside", "diagram.png", {},
                                           {.support = Support::OutsideApp, .supportDetail = "image content"})));
        CHECK(store.appendFile(FileSession("unreadable", "broken.txt", {}, {.support = Support::Unreadable})));
        test_support::awaitRecovery(store);
        const std::string bytes = std::string(":literal\t") + static_cast<char>(0xff);
        CHECK(store.appendFile(FileSession("bytes", bytes, draft())));
        test_support::awaitRecovery(store);
        const std::vector<std::string> displayOrder{"undecided", "bytes", "published", "outside", "unreadable"};
        for (int row = 0; row < static_cast<int>(displayOrder.size()); ++row) {
            const auto &expected = displayOrder[static_cast<std::size_t>(row)];
            CHECK(rows.rowOf(expected) == row);
            CHECK(rows.fileAt(row)->id() == expected);
            CHECK(role(rows, SessionModel::FileIdRole, row).toString().toStdString() == expected);
        }
        CHECK(rows.rowOf("missing") == -1 && store.rowOf("missing") == -1);
        CHECK(store.rowOf("bytes") == 4 && rows.rowOf("bytes") == 1);
        CHECK(store.rowOf("published") == 1 && rows.rowOf("published") == 2);
        CHECK(store.file("bytes") == rows.fileAt(rows.rowOf("bytes")));
        CHECK(store.supportCount(Support::Supported) == 3 && store.supportCount(Support::OutsideApp) == 1
              && store.supportCount(Support::Unreadable) == 1);
        const auto at = [&](const std::string &id, int value) { return role(rows, value, rows.rowOf(id)); };

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
        for (int row = 0; row < rows.rowCount(); ++row) {
            CHECK(!role(rows, SessionModel::SummaryRole, row).toString().contains(QStringLiteral(" \u00b7 ")));
            const auto chips = role(rows, SessionModel::ChipsRole, row).toList();
            CHECK(chips.size() >= 5);
            // The first chip is the status sentence, without an icon.
            CHECK(chips.first().toMap().value("text") == role(rows, SessionModel::StatusRole, row));
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
        CHECK(rows.displayPath("bytes") == escaped && rows.displayPath("missing").isEmpty());
        CHECK(store.file("bytes")->path() == bytes);
        CHECK(handoffEntries(SessionHandoff::build(store, {}).toVariant()).value("bytes").value("path").toString()
              == escaped);
        const auto blocked = finishBlockers(store);
        CHECK(blocked.contains("bytes") && blocked.value("bytes").value("path").toString() == escaped);
    });
    suite.run("Qt history restores atomic decisions, invalidates review and fences discard callbacks", [] {
        auto [store, rows] = test_support::TestSession{};
        CHECK(store.appendFile(FileSession("id", "file", draft())));
        test_support::awaitRecovery(store);
        resolve(store, "id");
        auto changed = draft("new result");
        changed.decisions["left"] = {DecisionValue::Ignored, {}};
        CHECK(await(store.replaceDraft("id", 1, changed)) == Transition::Applied);
        CHECK(role(rows, SessionModel::CanUndoRole).toBool());
        CHECK(store.file("id")->facts().earlierStaged && !SessionHandoff::build(store, {}).clean);
        CHECK(await(store.undo("id", 2)) == Transition::Applied);
        CHECK(store.file("id")->current().draft.text == "result\n");
        CHECK(role(rows, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "accepted");
        CHECK(role(rows, SessionModel::CanRedoRole).toBool());
        CHECK(role(rows, SessionModel::ReviewNeededRole).toBool() && !SessionHandoff::build(store, {}).clean);
        CHECK(await(store.redo("id", 3)) == Transition::Applied);
        CHECK(store.file("id")->current().draft.text == "new result");
        CHECK(role(rows, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "ignored");
        CHECK(await(store.discardToCheckpoint("id", 4)) == Transition::Applied);
        CHECK(await(store.recordCheckpoint("id", 2, Outcome::Succeeded)) == Transition::StaleRevision);
        CHECK(*store.file("id")->publication().checkpoint() == 1);
        CHECK(await(store.discardToStaged("id", 5)) == Transition::Applied);
        CHECK(SessionHandoff::build(store, {}).clean
              && role(rows, SessionModel::CurrentRevisionRole).toString() == "1");
        CHECK(await(store.recordCheckpoint("id", 4, Outcome::Failed)) == Transition::StaleRevision);
        CHECK(SessionHandoff::build(store, {}).clean);
    });
    suite.run("a settled file reads ready to mark resolved until undo reopens a choice", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        auto [store, rows] = test_support::TestSession(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::markedFile()));
        test_support::awaitRecovery(store);
        const auto row = [&] { return rows.rowOf("choices"); };
        const auto choose = [&](SourceSide side, std::size_t section) {
            const auto &file = *store.file("choices");
            const auto &target = test_support::choiceTarget(file, side, ChoiceScope::Section, section);
            return await(store.applyChoice(
                "choices",
                previewChoice(file, file.current().id, target.source.contributionId, ChoiceAction::ResolveUsingSide),
                true));
        };
        const auto settled = [&] {
            return role(rows, SessionModel::ProgressRole, row()).toMap().value("settled").toBool();
        };
        const auto line = [&] { return role(rows, SessionModel::StatusLineRole, row()).toString(); };
        CHECK(!settled() && line() != "Ready to mark resolved");
        CHECK(choose(SourceSide::Stage2, 0) == Transition::Applied);
        CHECK(!settled());
        CHECK(choose(SourceSide::Stage3, 1) == Transition::Applied);
        CHECK(settled() && line() == "Ready to mark resolved");
        // Ready is not reviewed or resolved, and the handoff keeps its existing category.
        CHECK(role(rows, SessionModel::StatusRole, row()).toString() == "Current result needs review");
        CHECK(!role(rows, SessionModel::ResolvedRole, row()).toBool());
        CHECK(await(store.undo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(!settled() && line() != "Ready to mark resolved");
        CHECK(await(store.redo("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(settled());
        store.recordRecoveryFailure("choices", ProblemCode::RecoveryStoreUnavailable);
        CHECK(!settled() && line() != "Ready to mark resolved");
    });
    suite.run("unconfirmed choices and marker blockers are visible without mutation", [] {
        auto [store, rows] = test_support::TestSession{};
        auto undecided = draft();
        undecided.decisions["left"] = {};
        CHECK(store.appendFile(FileSession("id", "file", undecided)));
        test_support::awaitRecovery(store);
        int changes = 0;
        QObject::connect(&rows, &SessionModel::summaryChanged, &store, [&] { ++changes; });
        CHECK(await(store.beginMarkResolved("id", 1)) == Transition::ConfirmationRequired);
        CHECK(changes == 0 && !store.file("id")->publication().reviewed());
        CHECK(role(rows, SessionModel::UndecidedRole).toULongLong() == 1);
        CHECK(await(store.beginMarkResolved("id", 1, true)) == Transition::Applied);
        CHECK(store.file("id")->publication().reviewed()->confirmedUndecided);
        undecided.text = "=======\n";
        CHECK(await(store.replaceDraft("id", 1, undecided)) == Transition::Applied);
        CHECK(await(store.beginMarkResolved("id", 2, true)) == Transition::Blocked);
        CHECK(changes == 4);
        CHECK(role(rows, SessionModel::FinishBlockersRole).toStringList().contains("Known conflict markers remain"));
    });
    for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain}) {
        suite.run("a delayed write stays visible after a newer Save: " + nameOf(outcome), [=] {
            auto [store, rows] = test_support::TestSession{};
            CHECK(store.appendFile(FileSession("id", "file", draft())));
            CHECK(await(store.beginMarkResolved("id", 1)) == Transition::Applied);
            CHECK(await(store.recordCheckpoint("id", 1, Outcome::Succeeded)) == Transition::Applied);
            CHECK(await(store.replaceDraft("id", 1, draft("new saved draft"))) == Transition::Applied);
            CHECK(await(store.recordCheckpoint("id", 2, Outcome::Succeeded)) == Transition::Applied);
            int changes = 0;
            QObject::connect(&rows, &SessionModel::summaryChanged, &store, [&] { ++changes; });
            CHECK(await(store.recordWrite("id", 1, outcome, "delayed diagnostic")) == Transition::Applied);
            CHECK(changes == 2);
            const auto &result = *store.file("id")->publication().writeOutcome();
            const auto status = outcome == Outcome::Succeeded ? "Written, unresolved"
                : outcome == Outcome::Failed                  ? "Repository write failed"
                                                              : "Outcome uncertain; verification required";
            CHECK(result.revision == 1);
            CHECK(result.outcome == outcome && result.detail == "delayed diagnostic");
            CHECK(role(rows, SessionModel::StatusRole).toString() == status);
            CHECK(role(rows, SessionModel::CurrentRevisionRole).toString() == "2");
            CHECK(*store.file("id")->publication().checkpoint() == 2);
            CHECK(store.file("id")->publication().reviewed()->revision == 1);
            CHECK(!role(rows, SessionModel::UnsavedRole).toBool()
                  && role(rows, SessionModel::ReviewNeededRole).toBool());
            CHECK(!SessionHandoff::build(store, {}).clean && rows.resolvedFiles() == 0
                  && !finishBlockers(store).isEmpty());
            const auto &written = store.file("id")->publication().written();
            CHECK(outcome == Outcome::Succeeded ? written == RevisionId{1} : !written);
        });
    }
    suite.run("typed edits are aligned on a worker as an immediate alignment would be", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        auto expected = *store.file("choices");
        CHECK(expected.editResult(1, {{0, 0}, "a"}) == Transition::Applied);
        CHECK(expected.editResult(2, {{1, 1}, "b"}) == Transition::Applied);
        CHECK(store.typeResult("choices", 1, {{0, 0}, "a"}) == Transition::Applied);
        CHECK(store.typeResult("choices", 2, {{1, 1}, "b"}) == Transition::Applied);
        CHECK(store.file("choices")->pendingAlignment() != nullptr);
        test_support::waitUntil([&] { return !store.file("choices")->pendingAlignment(); },
                                "the typed edits were not aligned");
        // Whichever revision the worker began with, only the current one's map is installed.
        const auto &typed = *store.file("choices");
        CHECK(typed.current().id == expected.current().id && typed.correspondence()->aligned());
        CHECK(typed.correspondence()->revision() == typed.current().id);
        for (const auto side : {SourceSide::Stage2, SourceSide::Stage3})
            CHECK(*typed.correspondence()->passages(side) == *expected.correspondence()->passages(side));
        CHECK(typed.markers() == expected.markers());
        CHECK(typed.current().draft.unresolvedMarkers == expected.current().draft.unresolvedMarkers);
    });
    suite.run("a map made for an earlier revision is dropped, and settling aligns the current one", [] {
        QTemporaryDir directory;
        CHECK(directory.isValid());
        SessionStore store(directory.path() + "/recovery");
        CHECK(store.appendFile(test_support::choiceFile()));
        test_support::awaitRecovery(store);
        CHECK(store.typeResult("choices", 1, {{0, 0}, "a"}) == Transition::Applied);
        const auto earlier = store.file("choices")->pendingAlignment();
        CHECK(earlier != nullptr);
        const auto stale = std::make_shared<const CorrespondenceMap>(earlier->realigned());
        CHECK(store.typeResult("choices", 2, {{1, 1}, "b"}) == Transition::Applied);
        CHECK(store.installAlignment("choices", 2, stale) == Transition::StaleRevision);
        CHECK(store.file("choices")->pendingAlignment() != nullptr);
        CHECK(await(store.awaitAlignment("choices", store.file("choices")->current().id)) == Transition::Applied);
        CHECK(!store.file("choices")->pendingAlignment() && store.file("choices")->correspondence()->aligned());
        CHECK(await(store.awaitAlignment("choices", store.file("choices")->current().id)) == Transition::NoChange);
        CHECK(await(store.awaitAlignment("missing", 1)) == Transition::StaleRevision);
    });
    return suite.report();
}
