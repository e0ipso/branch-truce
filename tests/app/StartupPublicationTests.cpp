// Save, discard and Mark resolved outcomes that a restart must keep: publication and staging
// intents, failed and retried Saves and discards, and a deliberate leave.
#include "StartupRoutes.h"

using namespace startup_routes;

void startup_routes::addPublicationCases(test_support::Suite &suite, const QString &)
{
    for (const bool staging : {false, true}) {
        suite.run(
            std::string("repeated successful publications reopen without an interrupted earlier revision: ")
                + (staging ? "staging" : "publication"),
            [=] {
                Repository repo;
                repo.conflict("merge");
                ScopedEnvironment environment(repo.environment);
                QString location;
                Draft latest;
                {
                    Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
                    openController(controller, repo);
                    auto *model = controller.store();
                    const auto id = openedFile(controller).id();
                    location = model->recoveryDirectory();
                    for (const auto &bytes :
                         {QByteArray("first published revision\n"), QByteArray("second published revision\n")}) {
                        const auto current = model->file(id)->current();
                        latest = current.draft;
                        latest.text = bytes.toStdString();
                        CHECK(model->replaceDraft(id, current.id, latest) == Transition::Applied);
                        const auto &file = *model->file(id);
                        const auto revision = file.current().id;
                        CHECK(model->beginMarkResolved(id, revision, true) == Transition::Applied);
                        CHECK(model->recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
                        CHECK(model
                                  ->commitRecord(mutationRecoveryTransaction(
                                      file, {QUuid::createUuid().toString(), MutationKind::Publication, {}}))
                                  .succeeded);
                        writeFile(repo.path("tracked.txt"), bytes);
                        CHECK(model->recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
                        if (staging) {
                            CHECK(model
                                      ->commitRecord(mutationRecoveryTransaction(
                                          file, {QUuid::createUuid().toString(), MutationKind::Staging, {}}))
                                      .succeeded);
                            repo.git({"add", "--", "tracked.txt"});
                            const auto object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
                            CHECK(model->recordStage(id, revision, Outcome::Succeeded, object.toStdString(), "100644")
                                  == Transition::Applied);
                        }
                    }
                }
                const auto repositoryBefore = repo.snapshot();
                for (int reopening = 0; reopening < 2; ++reopening) {
                    Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
                    openController(controller, repo);
                    const auto &restored = openedFile(controller);
                    CHECK(controller.store()->recoveryDirectory() == location && restored.current().draft == latest);
                    CHECK(!controller.store()->hasRecoveryFailure(restored.id()));
                    CHECK(
                        fileRole(*controller.files(), restored, SessionModel::RecoveryErrorRole).toString().isEmpty());
                    CHECK(restored.written() && restored.written()->id == restored.current().id);
                    CHECK(restored.facts().resolved == staging);
                    CHECK(repo.snapshot() == repositoryBefore);
                    const auto replay = replayRecovery(storage::RecoveryStore(location));
                    CHECK(replay.succeeded && replay.mutations.size() == (staging ? 4 : 2));
                    for (const auto &mutation : replay.mutations) CHECK(mutation.completed);
                }
            });
    }
    for (const bool laterRevision : {false, true}) {
        suite.run(
            "an unmatched publication after an earlier success stays uncertain on reopen "
                + std::to_string(laterRevision),
            [=] {
                Repository repo;
                repo.conflict("merge");
                ScopedEnvironment environment(repo.environment);
                QString location;
                {
                    Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
                    openController(controller, repo);
                    auto *model = controller.store();
                    const auto id = openedFile(controller).id();
                    auto draft = model->file(id)->current().draft;
                    draft.text = "first successfully written result\n";
                    CHECK(model->replaceDraft(id, 1, draft) == Transition::Applied);
                    CHECK(model->beginMarkResolved(id, 2, true) == Transition::Applied);
                    CHECK(model->recordCheckpoint(id, 2, Outcome::Succeeded) == Transition::Applied);
                    location = model->recoveryDirectory();
                    CHECK(model
                              ->commitRecord(mutationRecoveryTransaction(
                                  *model->file(id), {QUuid::createUuid().toString(), MutationKind::Publication, {}}))
                              .succeeded);
                    writeFile(repo.path("tracked.txt"), QByteArray::fromStdString(draft.text));
                    CHECK(model->recordWrite(id, 2, Outcome::Succeeded) == Transition::Applied);
                    if (laterRevision) {
                        draft.text = "later interrupted result\n";
                        CHECK(model->replaceDraft(id, 2, draft) == Transition::Applied);
                        CHECK(model->beginMarkResolved(id, 3, true) == Transition::Applied);
                        CHECK(model->recordCheckpoint(id, 3, Outcome::Succeeded) == Transition::Applied);
                    }
                    CHECK(model
                              ->commitRecord(mutationRecoveryTransaction(
                                  *model->file(id), {QUuid::createUuid().toString(), MutationKind::Publication, {}}))
                              .succeeded);
                }
                const auto before = repo.snapshot();
                Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
                openController(controller, repo);
                const auto &restored = openedFile(controller);
                const auto diagnostic =
                    fileRole(*controller.files(), restored, SessionModel::RecoveryErrorRole).toString();
                CHECK(controller.store()->hasRecoveryFailure(restored.id()));
                CHECK(diagnostic.contains("Interrupted publication") && diagnostic.contains("uncertain"));
                CHECK(!restored.facts().resolved && repo.snapshot() == before);
                const auto replay = replayRecovery(storage::RecoveryStore(location));
                CHECK(replay.succeeded && replay.mutations.size() == 2);
                CHECK(replay.mutations[0].completed && !replay.mutations[1].completed);
            });
    }
    suite.run("exact staged evidence durably recovers a missing staging completion only once", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString location;
        QByteArray object;
        RevisionId revision = 0;
        {
            Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
            openController(controller, repo);
            auto *model = controller.store();
            const auto id = openedFile(controller).id();
            auto draft = model->file(id)->current().draft;
            draft.text = "reviewed staged result\n";
            CHECK(model->replaceDraft(id, 1, draft) == Transition::Applied);
            revision = model->file(id)->current().id;
            CHECK(model->beginMarkResolved(id, revision, true) == Transition::Applied);
            CHECK(model->recordCheckpoint(id, revision, Outcome::Succeeded) == Transition::Applied);
            writeFile(repo.path("tracked.txt"), QByteArray::fromStdString(draft.text));
            CHECK(model->recordWrite(id, revision, Outcome::Succeeded) == Transition::Applied);
            location = model->recoveryDirectory();
            CHECK(model
                      ->commitRecord(mutationRecoveryTransaction(
                          *model->file(id), {QUuid::createUuid().toString(), MutationKind::Staging, {}}))
                      .succeeded);
            repo.git({"add", "--", "tracked.txt"});
            object = repo.git({"rev-parse", ":tracked.txt"}).trimmed();
        }
        const auto before = repo.snapshot();
        const auto records = storage::RecoveryStore(location).read().transactions.size();
        for (int reopening = 0; reopening < 2; ++reopening) {
            Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
            openController(controller, repo);
            const auto &restored = openedFile(controller);
            CHECK(!controller.store()->hasRecoveryFailure(restored.id()) && restored.facts().resolved);
            CHECK(restored.staged() && restored.staged()->revision.id == revision);
            CHECK(restored.staged()->objectId == object.toStdString() && restored.staged()->mode == "100644");
            CHECK(restored.stageOutcome()->detail.find("verified against disk and index") != std::string::npos);
            CHECK(repo.snapshot() == before);
            const auto read = storage::RecoveryStore(location).read();
            CHECK(read.succeeded && read.transactions.size() == records + 1);
            CHECK(read.transactions.back().state.value("action") == "stage-outcome");
            const auto replay = replayRecovery(storage::RecoveryStore(location));
            CHECK(replay.succeeded && replay.mutations.size() == 1 && replay.mutations.front().completed);
        }
    });
    suite.run("Save and discard leave the repository untouched and restart keeps the baseline", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        const auto before = repo.snapshot();
        QString location;
        Draft baseline;
        Draft discarded;
        {
            Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
            openController(controller, repo);
            const auto id = QString::fromStdString(openedFile(controller).id());
            CHECK(controller.selection()->select(id));
            auto saved = openedFile(controller).current().draft;
            saved.text += "saved app draft\n";
            saved.decisions["kept"] = {DecisionValue::Accepted, 4};
            CHECK(controller.store()->replaceDraft(openedFile(controller).id(), openedFile(controller).current().id,
                                                   saved)
                  == Transition::Applied);
            CHECK(openedFile(controller).facts().unsaved && !openedFile(controller).checkpoint());
            CHECK(controller.commands()->saveDraft(id, QString::number(openedFile(controller).current().id)));
            baseline = openedFile(controller).current().draft;
            CHECK(openedFile(controller).checkpoint() && openedFile(controller).checkpoint()->draft == baseline);
            CHECK(!openedFile(controller).facts().unsaved && !openedFile(controller).written()
                  && !openedFile(controller).staged() && !openedFile(controller).reviewed());
            CHECK(controller.feedback()->message() == "Draft saved in Branch Truce; repository file unchanged.");
            CHECK(controller.selectedFile()->status() == "Draft saved in Branch Truce; repository file unchanged");
            CHECK(repo.snapshot() == before);
            auto edited = baseline;
            edited.text += "discarded edit\n";
            edited.decisions["kept"] = {DecisionValue::Ignored, {}};
            CHECK(controller.store()->replaceDraft(openedFile(controller).id(), openedFile(controller).current().id,
                                                   edited)
                  == Transition::Applied);
            discarded = openedFile(controller).current().draft;
            CHECK(openedFile(controller).facts().unsaved && openedFile(controller).checkpoint()->draft == baseline);
            CHECK(controller.commands()->discardUnsaved(id, QString::number(openedFile(controller).current().id)));
            CHECK(openedFile(controller).current().draft == baseline);
            CHECK(openedFile(controller).current().draft != discarded);
            CHECK(openedFile(controller).current().draft.decisions.at("kept").value == DecisionValue::Accepted);
            CHECK(!openedFile(controller).facts().unsaved && !openedFile(controller).written()
                  && !openedFile(controller).staged());
            CHECK(controller.feedback()->message()
                  == "Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.");
            CHECK(repo.snapshot() == before);
            location = controller.store()->recoveryDirectory();
            const auto read = storage::RecoveryStore(location).read();
            CHECK(read.succeeded && read.transactions.back().state.value("action") == "discard-to-checkpoint");
        }
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
        openController(controller, repo);
        const auto &restored = openedFile(controller);
        CHECK(controller.store()->recoveryDirectory() == location);
        CHECK(restored.current().draft == baseline && restored.current().draft != discarded);
        CHECK(restored.checkpoint() && restored.checkpoint()->draft == baseline);
        CHECK(!restored.facts().unsaved && !restored.written() && !restored.staged());
        CHECK(repo.snapshot() == before);
    });
    suite.run("a retried discard describes the discarded file after another file is selected", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        const auto faults = std::make_shared<test_support::SyncFaults>();
        Application controller{Application::Config{.gitExecutable = gitOnPath(),
                                                   .recoveryDirectory = newRecoveryDirectory(),
                                                   .correspondence = {},
                                                   .files = faults}};
        openController(controller, repo);
        const auto trackedId = QString::fromStdString(openedFile(controller, "tracked.txt").id());
        const auto otherId = QString::fromStdString(openedFile(controller, "excluded.txt").id());
        const auto revision = [&] { return QString::number(openedFile(controller, "tracked.txt").current().id); };
        CHECK(controller.selection()->select(trackedId));
        auto saved = openedFile(controller, "tracked.txt").current().draft;
        saved.text += "saved app draft\n";
        CHECK(controller.store()->replaceDraft(trackedId.toStdString(), revision().toULongLong(), saved)
              == Transition::Applied);
        CHECK(controller.commands()->saveDraft(trackedId, revision()));
        auto edited = saved;
        edited.text += "discard me\n";
        CHECK(controller.store()->replaceDraft(trackedId.toStdString(), revision().toULongLong(), edited)
              == Transition::Applied);
        {
            const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
            CHECK(!controller.commands()->discardUnsaved(trackedId, revision()));
            CHECK_EQ(faults->failures.load(), 1);
        }
        // The other file has no Save, so a message built from it would say
        // the initial result was restored.
        CHECK(controller.selection()->select(otherId));
        CHECK(openedFile(controller, "excluded.txt").facts().unsaved);
        CHECK(controller.commands()->retryPendingRecovery(trackedId));
        CHECK(controller.feedback()->message()
              == "Discarded unsaved changes. Draft saved in Branch Truce; repository file unchanged.");
        CHECK(openedFile(controller, "tracked.txt").current().draft == saved);
    });
    suite.run("a failed Save retries one transaction and restart keeps it off the repository", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        const auto before = repo.snapshot();
        QString location;
        Draft baseline;
        Draft edited;
        QString request;
        qsizetype records = 0;
        {
            const auto faults = std::make_shared<test_support::SyncFaults>();
            Application controller{Application::Config{.gitExecutable = gitOnPath(),
                                                       .recoveryDirectory = newRecoveryDirectory(),
                                                       .correspondence = {},
                                                       .files = faults}};
            openController(controller, repo);
            const auto id = QString::fromStdString(openedFile(controller).id());
            CHECK(controller.selection()->select(id));
            auto saved = openedFile(controller).current().draft;
            saved.text += "saved app draft\n";
            saved.decisions["kept"] = {DecisionValue::Accepted, 4};
            CHECK(controller.store()->replaceDraft(openedFile(controller).id(), openedFile(controller).current().id,
                                                   saved)
                  == Transition::Applied);
            CHECK(controller.commands()->saveDraft(id, QString::number(openedFile(controller).current().id)));
            baseline = openedFile(controller).current().draft;
            CHECK(openedFile(controller).checkpoint() && openedFile(controller).checkpoint()->draft == baseline);
            CHECK(repo.snapshot() == before);
            auto next = baseline;
            next.text += "retry me\n";
            next.decisions["kept"] = {DecisionValue::Ignored, {}};
            CHECK(
                controller.store()->replaceDraft(openedFile(controller).id(), openedFile(controller).current().id, next)
                == Transition::Applied);
            edited = openedFile(controller).current().draft;
            const auto revision = QString::number(openedFile(controller).current().id);
            {
                const test_support::ArmedFault fault(*faults, test_support::SyncFaults::Point::JournalSync);
                CHECK(!controller.commands()->saveDraft(id, revision));
                CHECK_EQ(faults->failures.load(), 1);
            }
            CHECK(controller.feedback()->message() == "Draft save failed. The edited result is still available.");
            CHECK(openedFile(controller).current().draft == edited);
            CHECK(openedFile(controller).current().draft.decisions.at("kept").value == DecisionValue::Ignored);
            CHECK(openedFile(controller).checkpoint() && openedFile(controller).checkpoint()->draft == baseline);
            CHECK(openedFile(controller).facts().unsaved && !openedFile(controller).written()
                  && !openedFile(controller).staged());
            CHECK(controller.selectedFile()->status() == "Draft save failed");
            CHECK(controller.selectedFile()->recoveryState() == FileStates::RecoveryState::SaveFailed);
            CHECK(!controller.selectedFile()->canEdit());
            CHECK(!controller.commands()->undo(id, revision) && !controller.commands()->discardUnsaved(id, revision));
            CHECK(openedFile(controller).current().draft == edited);
            CHECK(repo.snapshot() == before);
            const auto pending = storage::RecoveryStore(controller.store()->recoveryDirectory()).read();
            CHECK(pending.succeeded && pending.transactions.back().state.value("action") == "manual-save");
            records = pending.transactions.size();
            request = pending.transactions.back().id;
            CHECK(pending.transactions.back().state.value("written").isNull()
                  && pending.transactions.back().state.value("staged").isNull());
            CHECK(controller.commands()->retryPendingRecovery(id));
            CHECK(!controller.files()->hasPendingRecovery());
            CHECK(openedFile(controller).current().draft == edited);
            CHECK(openedFile(controller).checkpoint() && openedFile(controller).checkpoint()->draft == edited);
            CHECK(!openedFile(controller).facts().unsaved && !openedFile(controller).written()
                  && !openedFile(controller).staged());
            CHECK(controller.selectedFile()->status() == "Draft saved in Branch Truce; repository file unchanged");
            CHECK(controller.selectedFile()->canEdit());
            const auto committed = storage::RecoveryStore(controller.store()->recoveryDirectory()).read();
            CHECK(committed.succeeded && committed.transactions.size() == records);
            CHECK(committed.transactions.back().id == request);
            CHECK(committed.transactions.back().state.value("action") == "manual-save");
            CHECK(committed.transactions.back().state.value("written").isNull()
                  && committed.transactions.back().state.value("staged").isNull());
            CHECK(repo.snapshot() == before);
            location = controller.store()->recoveryDirectory();
        }
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
        openController(controller, repo);
        const auto &restored = openedFile(controller);
        CHECK(controller.store()->recoveryDirectory() == location);
        CHECK(restored.current().draft == edited && restored.current().draft != baseline);
        CHECK(restored.current().draft.decisions.at("kept").value == DecisionValue::Ignored);
        CHECK(restored.checkpoint() && restored.checkpoint()->draft == edited);
        CHECK(!restored.facts().unsaved && !restored.written() && !restored.staged());
        CHECK(repo.snapshot() == before);
    });
    suite.run("a deliberate leave keeps drafts, runs no Git command and ends the session once", [] {
        Repository repo(true);
        repo.conflict("merge");
        repo.offlineGuard();
        ScopedEnvironment environment(repo.environment);
        const auto before = repo.snapshot();
        Draft kept;
        {
            Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
            openController(controller, repo);
            const auto &tracked = openedFile(controller);
            auto edited = tracked.current().draft;
            edited.text += "kept app draft\n";
            CHECK(controller.store()->replaceDraft(tracked.id(), tracked.current().id, edited) == Transition::Applied);
            kept = openedFile(controller).current().draft;
            const auto excluded = QString::fromLatin1(QByteArray("excluded.txt").toHex());
            const auto summary = controller.completion()->handoffData();
            CHECK(!summary.value("clean").toBool() && summary.value("canLeave").toBool());
            CHECK(summary.value("leaveBlocker").toString().isEmpty());
            CHECK(summary.value("totalFiles").toInt() == 2 && summary.value("outstandingFiles").toInt() == 2);
            CHECK(summary.value("counts").toMap().value("outside-app").toInt() == 1);
            QStringList routes;
            for (const auto &group : summary.value("groups").toList())
                for (const auto &entry : group.toMap().value("entries").toList()) {
                    const auto fields = entry.toMap();
                    CHECK(fields.value("outstanding").toBool() && !fields.value("action").toString().isEmpty());
                    if (fields.value("fileId").toString() == excluded)
                        CHECK(fields.value("category").toString() == "outside-app");
                    // A route opens its own file, including the outside-app one.
                    CHECK(controller.selection()->select(fields.value("fileId").toString()));
                    CHECK(controller.selection()->fileId() == fields.value("fileId").toString());
                    routes << fields.value("fileId").toString();
                }
            CHECK(routes.size() == 2 && routes.contains(excluded));
            CHECK(!controller.completion()->finish());
            int ended = 0;
            QString outcome;
            bool clean = true;
            QObject::connect(controller.completion(), &SessionCompletion::sessionEnded, &controller,
                             [&](const QString &text, bool value) {
                                 ++ended;
                                 outcome = text;
                                 clean = value;
                             });
            const auto calls = repo.offline.calls();
            CHECK(controller.completion()->leave());
            // Git runs only as a coordinator task, so a leave that started one would leave it
            // pending once the calls the leave queued have run.
            QCoreApplication::sendPostedEvents();
            CHECK_EQ(controller.coordinator()->pendingTasks(), 0);
            for (const auto *process : controller.findChildren<QProcess *>())
                CHECK(process->state() == QProcess::NotRunning);
            CHECK(repo.offline.calls() == calls);
            CHECK(ended == 1 && !clean);
            CHECK(outcome
                  == "Left with 2 of 2 files outstanding. Drafts stay in Branch Truce; "
                     "the Git operation was not continued or committed.");
            CHECK(controller.completion()->outcome() == outcome);
            CHECK(!controller.completion()->handoffData().value("canLeave").toBool());
            CHECK(!controller.completion()->leave() && !controller.completion()->finish() && ended == 1);
            CHECK(openedFile(controller).current().draft == kept);
            CHECK(repo.snapshot() == before);
        }
        Application controller{Application::Config{gitOnPath(), newRecoveryDirectory(), {}}};
        openController(controller, repo);
        CHECK(openedFile(controller).current().draft == kept);
        CHECK(repo.snapshot() == before);
    });
}
