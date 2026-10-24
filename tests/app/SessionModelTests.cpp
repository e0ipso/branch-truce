#include "app/SessionModel.h"
#include "TestHarness.h"
#include "fixtures/RecoveryFaults.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QProcess>
#include <QPersistentModelIndex>
#include <QSet>
#include <QTemporaryDir>
#include <limits>
#include <cstdlib>

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
FileSession recoverableFile()
{
    FileSnapshots captured;
    captured.path = std::string(":literal\t") + static_cast<char>(0xff);
    captured.provenance.sessionId = "recovery-session";
    captured.provenance.worktreeRoot = "/disposable-worktree";
    captured.provenance.gitDirectory = "/disposable-worktree/.git";
    captured.provenance.stage2CommitId = "left-commit";
    captured.provenance.stage3CommitId = "right-commit";
    captured.conversion.attributes = {{"text", "set"}, {"eol", "crlf"}};
    captured.stage2 = SourceSnapshot{"left-object", "100644", "left\n",
        {"left\n", "left\r\n", {SnapshotFormat::Encoding::Utf8, false, SnapshotFormat::Newline::CrLf, true}},
        "left-commit", "Current checkout"};
    captured.stage3 = SourceSnapshot{"right-object", "100644", "right\n",
        {"right\n", "right\n", {}}, "right-commit", "Merged commit"};
    captured.launchResult.text = {"manual launch\n", "\xef\xbb\xbfmanual launch\r\n",
        {SnapshotFormat::Encoding::Utf8, true, SnapshotFormat::Newline::CrLf, true}};
    captured.launchResult.identity = {11, 22, 0100644, 18, 33, 44, 55, 66};
    return FileSession("recoverable", captured.path, draft(captured.launchResult.text.utf8),
        Support::Supported, {}, captured);
}
QByteArray payload(const storage::RecoveryStore &store, const QJsonValue &reference)
{
    QString error;
    const auto bytes = store.payload(reference.toString().toLatin1(), &error);
    CHECK(error.isEmpty());
    return bytes;
}
void checkDraft(const storage::RecoveryStore &store, const QJsonObject &encoded, const Draft &expected)
{
    CHECK(payload(store, encoded.value("text")) == QByteArray::fromStdString(expected.text));
    CHECK(encoded.value("unresolvedMarkers").toString() == QString::number(expected.unresolvedMarkers));
    const auto decisions = encoded.value("decisions").toArray();
    CHECK(decisions.size() == static_cast<qsizetype>(expected.decisions.size()));
    for (const auto &value : decisions) {
        const auto decision = value.toObject();
        const auto id = payload(store, decision.value("id")).toStdString();
        CHECK(expected.decisions.contains(id));
        const auto &wanted = expected.decisions.at(id);
        CHECK(decision.value("value").toInt() == static_cast<int>(wanted.value));
        if (wanted.order) CHECK(decision.value("order").toString() == QString::number(*wanted.order));
        else CHECK(!decision.contains("order"));
    }
}
Draft savedDraft()
{
    auto value = draft("saved combined \xc3\xa9\n");
    value.decisions["right"] = {DecisionValue::Ignored, {}};
    return value;
}
Draft unsavedDraft()
{
    auto value = savedDraft();
    value.text += "acknowledged unsaved \xf0\x9f\x98\x80\n";
    value.decisions["right"] = {DecisionValue::Accepted, 2};
    value.unresolvedMarkers = 1;
    return value;
}
Draft finalDecision()
{
    auto value = unsavedDraft();
    value.decisions["later contribution"] = {DecisionValue::Ignored, {}};
    return value;
}
int acknowledgeAndCrash(const QString &directory)
{
    SessionModel model(directory);
    CHECK(model.appendFile(recoverableFile()));
    CHECK(model.replaceDraft("recoverable", 1, savedDraft()) == Transition::Applied);
    CHECK(model.recordCheckpoint("recoverable", 2, Outcome::Succeeded) == Transition::Applied);
    CHECK(model.replaceDraft("recoverable", 2, unsavedDraft()) == Transition::Applied);
    CHECK(model.replaceDraft("recoverable", 3, finalDecision()) == Transition::Applied);
    CHECK(!model.hasPendingRecovery() && model.file("recoverable")->current().id == 4);
    // Exit immediately after the acknowledgment without destructors or Qt cleanup.
    CHECK(::write(STDOUT_FILENO, "ACK 4\n", 6) == 6);
    std::_Exit(0);
}
}

class SessionModelTests {
public:
    int run()
    {
        test_support::Suite suite("SessionModelTests");
        suite.run("TRUCE-0013 an immediate process exit after acknowledgment preserves edits decisions captures and Save separately", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            const auto location = directory.path() + "/recovery";
            QProcess process;
            process.start(QCoreApplication::applicationFilePath(), {"--acknowledge-and-crash", location});
            CHECK(process.waitForStarted(5000) && process.waitForFinished(10000));
            CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
            CHECK(process.readAllStandardOutput() == "ACK 4\n");
            const storage::RecoveryStore reopened(location);
            const auto recovered = reopened.read();
            CHECK(recovered.succeeded && !recovered.incompleteTail && recovered.transactions.size() == 5);
            const auto last = recovered.transactions.back();
            const auto current = last.value("current").toObject();
            CHECK(last.value("schema").toInt() == 1 && !last.value("transactionId").toString().isEmpty());
            CHECK(current.value("id") == "4" && current.value("result") == "3" && current.value("decisions") == "4");
            checkDraft(reopened, current.value("draft").toObject(), finalDecision());
            const auto checkpoint = last.value("checkpoint").toObject();
            CHECK(checkpoint.value("id") == "2");
            checkDraft(reopened, checkpoint.value("draft").toObject(), savedDraft());
            CHECK(last.value("reviewed").isNull() && last.value("written").isNull() && last.value("staged").isNull());
            const auto expected = recoverableFile();
            CHECK(payload(reopened, last.value("path")) == QByteArray::fromStdString(expected.path()));
            const auto snapshots = last.value("snapshots").toObject();
            CHECK(snapshots.value("earlier").isNull());
            const auto left = snapshots.value("stage2").toObject();
            CHECK(payload(reopened, left.value("rawBytes")) == "left\n");
            CHECK(payload(reopened, left.value("objectId")) == "left-object");
            CHECK(payload(reopened, left.value("worktree").toObject().value("bytes")) == "left\r\n");
            const auto right = snapshots.value("stage3").toObject();
            CHECK(payload(reopened, right.value("rawBytes")) == "right\n");
            CHECK(payload(reopened, right.value("objectId")) == "right-object");
            CHECK(payload(reopened, right.value("commitId")) == "right-commit");
            const auto launch = snapshots.value("launchResult").toObject();
            CHECK(payload(reopened, launch.value("text").toObject().value("bytes")) == "\xef\xbb\xbfmanual launch\r\n");
            CHECK(launch.value("text").toObject().value("format").toObject().value("bom").toBool());
            CHECK(launch.value("identity").toObject().value("inode") == "22");
            const auto provenance = snapshots.value("provenance").toObject();
            CHECK(payload(reopened, provenance.value("sessionId")) == "recovery-session");
            CHECK(payload(reopened, provenance.value("worktreeRoot")) == "/disposable-worktree");
            CHECK(payload(reopened, provenance.value("gitDirectory")) == "/disposable-worktree/.git");
            const auto attributes = snapshots.value("conversion").toObject().value("attributes").toArray();
            QMap<QByteArray, QByteArray> policy;
            for (const auto &attribute : attributes)
                policy.insert(payload(reopened, attribute.toObject().value("key")),
                    payload(reopened, attribute.toObject().value("value")));
            CHECK(policy == QMap<QByteArray, QByteArray>{{"eol", "crlf"}, {"text", "set"}});
        });
        suite.run("TRUCE-0013 failed recovery retains pending input separately and retries its completed record once", [] {
            QTemporaryDir directory;
            CHECK(directory.isValid());
            SessionModel model(directory.path() + "/recovery");
            CHECK(model.appendFile(recoverableFile()));
            const auto original = model.file("recoverable")->current().draft;
            recovery_faults::Failure fault(recovery_faults::Point::JournalSync);
            CHECK(model.replaceDraft("recoverable", 1, unsavedDraft()) == Transition::Blocked);
            CHECK(recovery_faults::failures == 1 && model.hasPendingRecovery());
            CHECK(model.file("recoverable")->current().id == 1 && model.file("recoverable")->current().draft == original);
            CHECK(model.pendingDraft("recoverable") && *model.pendingDraft("recoverable") == unsavedDraft());
            CHECK(role(model, SessionModel::PendingInputRole).toBool());
            CHECK(role(model, SessionModel::PendingTextRole).toString().toUtf8() == QByteArray::fromStdString(unsavedDraft().text));
            CHECK(role(model, SessionModel::PendingDecisionsRole).toList().size() == 2);
            CHECK(role(model, SessionModel::RecoveryStateRole).toString() == "failed");
            CHECK(role(model, SessionModel::RecoveryErrorRole).toString().contains("retry"));
            CHECK(!role(model, SessionModel::CanUndoRole).toBool() && !model.canFinish());
            CHECK(model.replaceDraft("recoverable", 1, draft("must not overwrite pending")) == Transition::Blocked);
            CHECK(model.recordCheckpoint("recoverable", 1, Outcome::Succeeded) == Transition::Blocked);
            CHECK(model.undo("recoverable", 1) == Transition::Blocked);
            storage::RecoveryStore store(model.recoveryDirectory());
            const auto before = store.read();
            CHECK(before.succeeded && before.transactions.size() == 2);
            const auto request = before.transactions.back().value("transactionId");
            CHECK(model.retryPendingRecovery("recoverable"));
            CHECK(!model.hasPendingRecovery() && !model.pendingDraft("recoverable"));
            CHECK(model.file("recoverable")->current().id == 2 && model.file("recoverable")->current().draft == unsavedDraft());
            CHECK(role(model, SessionModel::RecoveryStateRole).toString() == "durable");
            CHECK(role(model, SessionModel::RecoveryErrorRole).toString().isEmpty());
            CHECK(model.file("recoverable")->facts().unsaved && !model.file("recoverable")->checkpoint());
            const auto after = store.read();
            CHECK(after.succeeded && after.transactions.size() == 2 && after.transactions.back().value("transactionId") == request);
            checkDraft(store, after.transactions.back().value("current").toObject().value("draft").toObject(), unsavedDraft());
            CHECK(model.undo("recoverable", 2) == Transition::Applied);
            CHECK(model.file("recoverable")->current().draft == original);
        });
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
    if (app.arguments().size() == 3 && app.arguments()[1] == "--acknowledge-and-crash")
        return acknowledgeAndCrash(app.arguments()[2]);
    QTemporaryDir data;
    if (!data.isValid()) return 2;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    return SessionModelTests{}.run();
}
