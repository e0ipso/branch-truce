#include "app/SessionModel.h"
#include "TestHarness.h"
#include <QCoreApplication>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTemporaryDir>
#include <limits>

using namespace branch_truce;

namespace {
Draft draft(std::string text = "result\n")
{
    return {std::move(text), {{"left", {DecisionValue::Accepted, 1}}}, 0};
}
QVariant role(const SessionModel &model, int value, int row = 0)
{
    return model.data(model.index(row), value);
}
void resolve(SessionModel &model, const std::string &id)
{
    const auto revision = model.file(id)->current().id;
    CHECK(model.beginMarkResolved(id, revision) == Transition::Applied);
    CHECK(model.recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.recordStage(id, revision, Outcome::Succeeded, "verified-object", "100644") == Transition::Applied);
}
}

class SessionModelTests {
public:
    int run()
    {
        test_support::Suite suite("SessionModelTests");
        suite.run("empty model and invalid indexes", [] {
            SessionModel model;
            CHECK(model.totalFiles() == 0 && model.rowCount() == 0 && model.resolvedFiles() == 0);
            CHECK(!model.canFinish() && model.finishBlockers().size() == 1);
            CHECK(model.finishBlockers().first().toMap().value("blockers").toStringList()
                  == QStringList{"No files loaded"});
            CHECK(!model.file("missing") && !model.data({}, SessionModel::PathRole).isValid());
            CHECK(model.appendFile(FileSession("id", "file", draft())));
            CHECK(model.rowCount(model.index(0)) == 0 && !model.index(1).isValid());
            CHECK(!model.data(model.index(0), -1).isValid());
            SessionModel other;
            CHECK(other.appendFile(FileSession("other", "path", draft())));
            CHECK(!model.data(other.index(0), SessionModel::PathRole).isValid());
        });
        suite.run("named roles preserve byte paths, Unicode and 64-bit decision order", [] {
            SessionModel model;
            auto unicode = draft("A\xc3\xa9\xf0\x9f\x98\x80\n");
            unicode.decisions["left"].order = std::numeric_limits<std::uint64_t>::max();
            unicode.decisions["right"] = {DecisionValue::Ignored, {}};
            const std::string path = std::string(":literal\t\n") + static_cast<char>(0xff);
            CHECK(model.appendFile(FileSession("id", path, unicode)));
            CHECK(role(model, SessionModel::PathBytesRole).toByteArray()
                  == QByteArray(path.data(), static_cast<qsizetype>(path.size())));
            CHECK(role(model, SessionModel::ResultTextRole).toString().toUtf8()
                  == QByteArray(unicode.text.data(), static_cast<qsizetype>(unicode.text.size())));
            for (const auto value : {SessionModel::CurrentRevisionRole, SessionModel::ResultRevisionRole,
                                     SessionModel::DecisionRevisionRole}) {
                CHECK(role(model, value).metaType() == QMetaType::fromType<QString>());
                CHECK(role(model, value).toString() == "1");
            }
            const auto decisions = role(model, SessionModel::DecisionsRole).toList();
            CHECK(decisions.size() == 2);
            CHECK(decisions[0].toMap().value("value").toString() == "accepted");
            CHECK(decisions[0].toMap().value("order").toString() == "18446744073709551615");
            CHECK(decisions[1].toMap().value("value").toString() == "ignored");
            CHECK(!decisions[1].toMap().contains("order"));
            const auto names = model.roleNames();
            CHECK(names.size() == SessionModel::RecoveryErrorRole - SessionModel::FileIdRole + 1);
            QSet<QByteArray> unique;
            for (auto it = names.cbegin(); it != names.cend(); ++it) {
                CHECK(!it.value().isEmpty() && !unique.contains(it.value()));
                unique.insert(it.value());
            }
            CHECK(names.value(SessionModel::CheckpointRole) == "manualCheckpoint");
            CHECK(names.value(SessionModel::WriteOutcomeRole) == "writeOutcome");
        });
        suite.run("row insertion and notifications occur only for applied commands", [] {
            SessionModel model;
            int summaries = 0;
            int changes = 0;
            int insertions = 0;
            QObject::connect(&model, &SessionModel::summaryChanged, &model, [&] { ++summaries; });
            QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                [&](const QModelIndex &parent, int first, int last) {
                    CHECK(!parent.isValid() && first == last);
                    ++insertions;
                });
            QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
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
            SessionModel model;
            CHECK(model.appendFile(FileSession("first", "a", draft())));
            CHECK(model.recordCheckpoint("first", 1, Outcome::Succeeded) == Transition::Applied);
            CHECK(!role(model, SessionModel::UnsavedRole).toBool());
            CHECK(role(model, SessionModel::ReviewNeededRole).toBool());
            CHECK(role(model, SessionModel::ReviewedRole).toMap().isEmpty());
            CHECK(role(model, SessionModel::WrittenRole).toMap().isEmpty());
            CHECK(role(model, SessionModel::StagedRole).toMap().isEmpty());
            CHECK(role(model, SessionModel::CheckpointRole).toMap().value("text").toString() == "result\n");
            resolve(model, "first");
            CHECK(model.canFinish() && model.resolvedFiles() == 1 && model.finishBlockers().isEmpty());
            CHECK(role(model, SessionModel::StagedRole).toMap().value("objectId").toString() == "verified-object");
            CHECK(role(model, SessionModel::StagedRole).toMap().value("mode").toString() == "100644");
            CHECK(model.appendFile(FileSession("second", "b", draft(), Support::OutsideApp, "binary")));
            CHECK(model.totalFiles() == 2 && model.resolvedFiles() == 1 && !model.canFinish());
            CHECK(role(model, SessionModel::SupportRole, 1).toString() == "outside-app");
            CHECK(role(model, SessionModel::SupportDetailRole, 1).toString() == "binary");
            CHECK(model.finishBlockers().size() == 1);
            CHECK(model.finishBlockers()[0].toMap().value("fileId").toString() == "second");
            CHECK(model.setSupport("second", Support::Unreadable, "read failed") == Transition::Applied);
            CHECK(role(model, SessionModel::SupportRole, 1).toString() == "unreadable");
            CHECK(!model.canFinish());
            CHECK(model.setSupport("second", Support::Supported) == Transition::Applied);
            resolve(model, "second");
            CHECK(model.canFinish() && model.resolvedFiles() == 2);
            CHECK(model.recordCheckpoint("second", 1, Outcome::Failed, "disk full") == Transition::Applied);
            CHECK(!model.canFinish() && model.resolvedFiles() == 1);
            CHECK(role(model, SessionModel::CheckpointOutcomeRole, 1).toMap().value("detail").toString() == "disk full");
            CHECK(role(model, SessionModel::StatusRole, 1).toString() == "Draft save failed");
        });
        suite.run("Qt history restores atomic decisions, invalidates review and fences discard callbacks", [] {
            SessionModel model;
            CHECK(model.appendFile(FileSession("id", "file", draft())));
            resolve(model, "id");
            auto changed = draft("new result");
            changed.decisions["left"] = {DecisionValue::Ignored, {}};
            CHECK(model.replaceDraft("id", 1, changed) == Transition::Applied);
            CHECK(role(model, SessionModel::CanUndoRole).toBool());
            CHECK(role(model, SessionModel::EarlierStagedRole).toBool() && !model.canFinish());
            CHECK(model.undo("id", 2) == Transition::Applied);
            CHECK(role(model, SessionModel::ResultTextRole).toString() == "result\n");
            CHECK(role(model, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "accepted");
            CHECK(role(model, SessionModel::CanRedoRole).toBool());
            CHECK(role(model, SessionModel::ReviewNeededRole).toBool() && !model.canFinish());
            CHECK(model.redo("id", 3) == Transition::Applied);
            CHECK(role(model, SessionModel::ResultTextRole).toString() == "new result");
            CHECK(role(model, SessionModel::DecisionsRole).toList()[0].toMap().value("value").toString() == "ignored");
            CHECK(model.discardToCheckpoint("id", 4) == Transition::Applied);
            CHECK(model.recordCheckpoint("id", 2, Outcome::Succeeded) == Transition::StaleRevision);
            CHECK(role(model, SessionModel::CheckpointRole).toMap().value("revision").toString() == "1");
            CHECK(model.discardToStaged("id", 5) == Transition::Applied);
            CHECK(model.canFinish() && role(model, SessionModel::CurrentRevisionRole).toString() == "1");
            CHECK(model.recordCheckpoint("id", 4, Outcome::Failed) == Transition::StaleRevision);
            CHECK(model.canFinish());
        });
        suite.run("unconfirmed choices and marker blockers are visible without mutation", [] {
            SessionModel model;
            auto undecided = draft();
            undecided.decisions["left"] = {};
            CHECK(model.appendFile(FileSession("id", "file", undecided)));
            int changes = 0;
            QObject::connect(&model, &SessionModel::summaryChanged, &model, [&] { ++changes; });
            CHECK(model.beginMarkResolved("id", 1) == Transition::ConfirmationRequired);
            CHECK(changes == 0 && role(model, SessionModel::ReviewedRole).toMap().isEmpty());
            CHECK(role(model, SessionModel::UndecidedRole).toULongLong() == 1);
            CHECK(model.beginMarkResolved("id", 1, true) == Transition::Applied);
            CHECK(role(model, SessionModel::ReviewedRole).toMap().value("confirmedUndecided").toBool());
            undecided.unresolvedMarkers = 1;
            CHECK(model.replaceDraft("id", 1, undecided) == Transition::Applied);
            CHECK(model.beginMarkResolved("id", 2, true) == Transition::Blocked);
            CHECK(changes == 2);
            CHECK(role(model, SessionModel::FinishBlockersRole).toStringList().contains("Known conflict markers remain"));
        });
        for (const auto outcome : {Outcome::Succeeded, Outcome::Failed, Outcome::Uncertain}) {
            suite.run("TRUCE-0042 delayed write stays visible after a newer Save "
                      + std::to_string(static_cast<int>(outcome)), [=] {
                SessionModel model;
                CHECK(model.appendFile(FileSession("id", "file", draft())));
                CHECK(model.beginMarkResolved("id", 1) == Transition::Applied);
                CHECK(model.recordCheckpoint("id", 1, Outcome::Succeeded) == Transition::Applied);
                CHECK(model.replaceDraft("id", 1, draft("new saved draft")) == Transition::Applied);
                CHECK(model.recordCheckpoint("id", 2, Outcome::Succeeded) == Transition::Applied);
                int changes = 0;
                QObject::connect(&model, &SessionModel::summaryChanged, &model, [&] { ++changes; });
                CHECK(model.recordWrite("id", 1, outcome, "delayed diagnostic") == Transition::Applied);
                CHECK(changes == 1);
                const auto result = role(model, SessionModel::WriteOutcomeRole).toMap();
                const auto name = outcome == Outcome::Succeeded ? "succeeded" : outcome == Outcome::Failed ? "failed" : "uncertain";
                const auto status = outcome == Outcome::Succeeded ? "Written, unresolved" : outcome == Outcome::Failed
                    ? "Repository write failed" : "Outcome uncertain; verification required";
                CHECK(result.value("revision").toString() == "1");
                CHECK(result.value("outcome").toString() == name && result.value("detail").toString() == "delayed diagnostic");
                CHECK(role(model, SessionModel::StatusRole).toString() == status);
                CHECK(role(model, SessionModel::CurrentRevisionRole).toString() == "2");
                CHECK(role(model, SessionModel::CheckpointRole).toMap().value("revision").toString() == "2");
                CHECK(role(model, SessionModel::ReviewedRole).toMap().value("revision").toString() == "1");
                CHECK(!role(model, SessionModel::UnsavedRole).toBool() && role(model, SessionModel::ReviewNeededRole).toBool());
                CHECK(!model.canFinish() && model.resolvedFiles() == 0 && !model.finishBlockers().isEmpty());
                const auto written = role(model, SessionModel::WrittenRole).toMap();
                CHECK(outcome == Outcome::Succeeded ? written.value("revision").toString() == "1" : written.isEmpty());
            });
        }
        return suite.report();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir data;
    if (!data.isValid()) return 2;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    return SessionModelTests{}.run();
}
