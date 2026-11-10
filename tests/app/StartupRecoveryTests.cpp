// Startup recovery: restoring acknowledged work, incomplete records, retired and unadopted stores,
// several windows and the recovery-session choice, and external resolution while closed.
#include "StartupRoutes.h"
#include "fixtures/AppSession.h"
#include "fixtures/Await.h"

using test_support::await;
using test_support::awaitAligned;
using test_support::commandResult;

using namespace startup_routes;

int startup_routes::openEditAndCrash(const QString &root)
{
    Application controller{test_support::windowConfig()};
    if (!controller.loader()->openRepository(root)) return 3;
    QEventLoop loop;
    QObject::connect(controller.loader(), &SessionLoader::launchChanged, &loop, [&] {
        if (!controller.loader()->opening()) loop.quit();
    });
    QTimer::singleShot(test_support::kWaitDeadlineMs, &loop, &QEventLoop::quit);
    if (controller.loader()->opening()) loop.exec();
    if (controller.loader()->startupState() != SessionLoader::StartupState::Ready) return 4;
    const auto *file = controller.store()->file(test_support::fileIdOf("tracked.txt"));
    if (!file || !controller.store()->recoveryLease()) return 5;
    if (awaitAligned(
            *controller.store(),
            controller.store()->editResult(file->id(), file->current().id, {{0, 0}, "edited before the crash\n"}))
        != Transition::Applied)
        return 6;
    std::fputs("ACK\n", stdout);
    std::fflush(stdout);
    // Ends the process the way a crash does: no destructor runs, so the lease's lock file stays.
    ::kill(::getpid(), SIGKILL);
    return 7;
}

void startup_routes::addRecoveryCases(test_support::Suite &suite, const QString &application)
{
    suite.run("a window killed while it holds the session lock leaves its edit for the next start", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QProcess crashed;
        crashed.setProcessEnvironment(repo.environment);
        crashed.start(QCoreApplication::applicationFilePath(), {"--open-edit-and-crash", repo.root});
        CHECK(crashed.waitForStarted(5000));
        test_support::waitUntil([&] { return crashed.state() == QProcess::NotRunning; },
                                "the window that crashes did not end");
        CHECK(crashed.exitStatus() == QProcess::CrashExit);
        CHECK_EQ(crashed.readAllStandardOutput(), QByteArray("ACK\n"));
        // The killed window left its store and the lock file of its lease behind.
        const auto stores = QDir(recoveryRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        CHECK_EQ(stores.size(), 1);
        const auto location = recoveryRoot() + "/" + stores.front();
        CHECK(QFileInfo::exists(location + "/session.lock"));
        Application controller{test_support::windowConfig()};
        openController(controller, repo);
        CHECK(controller.loader()->recoveryChoices().isEmpty());
        CHECK_EQ(controller.store()->recoveryDirectory(), location);
        CHECK(controller.store()->recoveryLease() != nullptr);
        CHECK(openedFile(controller).current().draft.text.starts_with("edited before the crash\n"));
        CHECK(readFile(repo.path("tracked.txt")) == repo.manual);
    });
    suite.run("restart restores acknowledged unsaved work independently of Save review and resolution", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString location;
        Draft saved, unsaved;
        std::optional<FileSnapshots> captured;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            auto *model = controller.store();
            const auto &selected = openedFile(controller);
            captured = *selected.snapshots();
            const auto end = selected.current().draft.text.size();
            CHECK(
                awaitAligned(*model,
                             model->editResult(selected.id(), selected.current().id, {{end, end}, "saved app draft\n"}))
                == Transition::Applied);
            saved = selected.current().draft;
            CHECK(await(model->recordCheckpoint(selected.id(), selected.current().id, Outcome::Succeeded))
                  == Transition::Applied);
            const auto savedEnd = saved.text.size();
            CHECK(awaitAligned(*model,
                               model->editResult(selected.id(), selected.current().id,
                                                 {{savedEnd, savedEnd}, "acknowledged unsaved work\n"}))
                  == Transition::Applied);
            unsaved = selected.current().draft;
            location = model->recoveryDirectory();
            CHECK(!model->hasPendingRecovery());
        }
        for (int restart = 0; restart < 2; ++restart) {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &restored = openedFile(controller);
            CHECK(controller.store()->recoveryDirectory() == location);
            CHECK(restored.current().draft == unsaved && *restored.snapshots() == *captured);
            CHECK(restored.publication().checkpoint()
                  && restored.revision(*restored.publication().checkpoint())->draft == saved);
            CHECK(restored.facts().unsaved && !restored.facts().resolved && restored.facts().needsReview);
            CHECK(!restored.publication().reviewed() && !restored.publication().written()
                  && !restored.publication().staged());
            CHECK(fileRole(*controller.files(), restored, SessionModel::RecoveryStateRole)
                      .value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Restored);
            CHECK(!fileRole(*controller.files(), restored, SessionModel::PendingInputRole).toBool());
            CHECK(!controller.store()->hasRecoveryFailures() && !controller.completion()->handoff().clean);
        }
    });
    suite.run("an incomplete final record restores only acknowledged work with a pending-input diagnostic", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString location;
        Draft acknowledged;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &selected = openedFile(controller);
            CHECK(awaitAligned(*controller.store(),
                               controller.store()->editResult(selected.id(), 1, {{0, 0}, "acknowledged\n"}))
                  == Transition::Applied);
            acknowledged = selected.current().draft;
            location = controller.store()->recoveryDirectory();
        }
        QFile journal(location + "/journal");
        CHECK(journal.open(QIODevice::Append));
        CHECK(journal.write("{\"interrupted pending input\":", 29) == 29);
        journal.close();
        Application controller{test_support::windowConfig()};
        openController(controller, repo);
        const auto &restored = openedFile(controller);
        CHECK(restored.current().draft == acknowledged && restored.facts().unsaved);
        CHECK(controller.loader()->detail().contains("Pending input may need repeating"));
        CHECK(!controller.files()->hasPendingRecovery() && !controller.store()->hasRecoveryFailures());
        CHECK(
            fileRole(*controller.files(), restored, SessionModel::RecoveryStateRole).value<FileStates::RecoveryState>()
            == FileStates::RecoveryState::Restored);
    });
    for (const auto &condition : {QString("stale"), QString("corrupt"), QString("missing-payload"),
                                  QString("corrupt-payload"), QString("schema"), QString("locked")}) {
        suite.run(
            "unadopted " + condition.toStdString() + " store keeps diagnostics and every live file editable", [=] {
                Repository repo;
                repo.conflict("merge");
                ScopedEnvironment environment(repo.environment);
                QString abandoned;
                const QByteArray oldDraft = "old acknowledged unsaved work\n";
                {
                    Application controller{test_support::windowConfig()};
                    openController(controller, repo);
                    const auto &selected = openedFile(controller);
                    auto edited = selected.current().draft;
                    edited.text = oldDraft.toStdString();
                    CHECK(await(controller.store()->replaceDraft(selected.id(), 1, edited)) == Transition::Applied);
                    abandoned = controller.store()->recoveryDirectory();
                }
                auto lock = std::make_unique<QLockFile>(abandoned + "/session.lock");
                QString diagnostic;
                if (condition == "stale") {
                    repo.git({"merge", "--abort"});
                    writeFile(repo.path("tracked.txt"), "second main\n");
                    repo.git({"add", "--", "tracked.txt"});
                    repo.git({"commit", "--quiet", "-m", "second operation"});
                    repo.git({"merge", "--no-edit", "side"}, 1);
                    writeFile(repo.path("tracked.txt"), "second manual result\n");
                    diagnostic = "session identity does not match";
                } else if (condition == "locked") {
                    CHECK(lock->tryLock(0));
                    diagnostic = "active or unavailable";
                } else if (condition == "schema") {
                    test_support::rewriteLastRecoveryState(abandoned,
                                                           [](QJsonObject &state) { state.insert("schema", 99); });
                    diagnostic = "schema";
                } else if (condition == "corrupt") {
                    QFile journal(abandoned + "/journal");
                    CHECK(journal.open(QIODevice::Append) && journal.write("corrupt complete frame\n") > 0);
                    diagnostic = "integrity";
                } else {
                    const auto path =
                        abandoned + "/payloads/" + QString::fromLatin1(storage::RecoveryStore::digest(oldDraft));
                    if (condition == "missing-payload")
                        CHECK(QFile::remove(path));
                    else
                        writeFile(path, "different payload bytes");
                    diagnostic = "payload";
                }
                Application controller{test_support::windowConfig()};
                openController(controller, repo);
                const auto detail = controller.loader()->detail();
                CHECK(detail.contains(diagnostic) && controller.store()->recoveryDirectory() != abandoned);
                CHECK(!controller.store()->hasRecoveryFailures() && !controller.files()->hasPendingRecovery());
                CHECK(openedFile(controller).current().draft.text != oldDraft.toStdString());
                for (const auto path : {"tracked.txt", "excluded.txt"}) {
                    const auto &selected = openedFile(controller, path);
                    CHECK(!controller.store()->hasRecoveryFailure(selected.id()));
                    const auto id = QString::fromStdString(selected.id());
                    CHECK(controller.selection()->select(id) && controller.selectedFile()->canEdit());
                    CHECK(commandResult(controller,
                                        controller.commands()->editResult(id, QString::number(selected.current().id), 0,
                                                                          0, "live manual edit\n")));
                    CHECK(commandResult(controller,
                                        controller.commands()->undo(id, QString::number(selected.current().id))));
                    CHECK(commandResult(controller,
                                        controller.commands()->redo(id, QString::number(selected.current().id))));
                    CHECK(controller.loader()->detail() == detail && !controller.files()->hasPendingRecovery());
                }
            });
    }
    suite.run("manual retirement outside v1 preserves the complete store and is reversible", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString abandoned;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            abandoned = controller.store()->recoveryDirectory();
        }
        QFile journal(abandoned + "/journal");
        CHECK(journal.open(QIODevice::Append) && journal.write("corrupt complete frame\n") > 0);
        journal.close();
        const auto retained = storeContents(abandoned);
        CHECK(retained.contains("journal") && retained.size() > 1);
        const auto retired = repo.executables.path() + "/retired-session";
        CHECK(!retired.startsWith(QFileInfo(abandoned).absolutePath() + '/'));
        CHECK(QDir().rename(abandoned, retired) && !QFileInfo::exists(abandoned));
        CHECK(storeContents(retired) == retained);
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            CHECK(!controller.loader()->detail().contains("rejected") && !controller.store()->hasRecoveryFailures());
            const auto &selected = openedFile(controller);
            CHECK(awaitAligned(
                      *controller.store(),
                      controller.store()->editResult(selected.id(), selected.current().id, {{0, 0}, "live edit\n"}))
                  == Transition::Applied);
        }
        CHECK(QDir().rename(retired, abandoned) && storeContents(abandoned) == retained);
        Application controller{test_support::windowConfig()};
        openController(controller, repo);
        CHECK(controller.loader()->detail().contains("integrity") && !controller.store()->hasRecoveryFailures());
        CHECK(openedFile(controller).current().draft.text.starts_with("live edit\n"));
    });
    suite.run("two windows closed before any edit leave later launches able to recover", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QStringList untouched;
        {
            Application first{test_support::windowConfig()};
            openController(first, repo);
            Application second{test_support::windowConfig()};
            openController(second, repo);
            untouched = {first.store()->recoveryDirectory(), second.store()->recoveryDirectory()};
            CHECK(untouched.front() != untouched.back());
            CHECK(second.loader()->detail().contains("active or unavailable"));
        }
        QString location;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            CHECK(!controller.loader()->detail().contains("Multiple recovery sessions"));
            CHECK(controller.loader()->detail().contains("without app edits that were deleted: 2"));
            CHECK(!untouched.contains(controller.store()->recoveryDirectory()));
            // A store holding only launch captures has nothing to restore later, so it is deleted.
            for (const auto &store : untouched) CHECK(!QFileInfo::exists(store));
            const auto &selected = openedFile(controller);
            CHECK(awaitAligned(*controller.store(),
                               controller.store()->editResult(selected.id(), selected.current().id,
                                                              {{0, 0}, "after two windows\n"}))
                  == Transition::Applied);
            location = controller.store()->recoveryDirectory();
        }
        for (int restart = 0; restart < 2; ++restart) {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            CHECK(!controller.loader()->detail().contains("Multiple recovery sessions"));
            CHECK(controller.loader()->detail().contains("Recovered app drafts"));
            CHECK(controller.store()->recoveryDirectory() == location);
            CHECK(openedFile(controller).current().draft.text.starts_with("after two windows\n"));
        }
    });
    suite.run("edits from two windows block restoration until the user chooses a session", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString first;
        QString second;
        {
            Application one{test_support::windowConfig()};
            openController(one, repo);
            Application two{test_support::windowConfig()};
            openController(two, repo);
            for (auto *controller : {&one, &two}) {
                const auto &selected = openedFile(*controller);
                const std::string text = controller == &one ? "first window\n" : "second window\n";
                CHECK(
                    awaitAligned(*controller->store(),
                                 controller->store()->editResult(selected.id(), selected.current().id, {{0, 0}, text}))
                    == Transition::Applied);
            }
            first = one.store()->recoveryDirectory();
            second = two.store()->recoveryDirectory();
        }
        const auto firstName = QFileInfo(first).fileName();
        const auto secondName = QFileInfo(second).fileName();
        const auto retained = storeContents(second);
        // The block holds across launches; each blocked launch opens the live files.
        for (int restart = 0; restart < 2; ++restart) {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto detail = controller.loader()->detail();
            CHECK(detail.contains("restoration is blocked rather than guessing")
                  && detail.contains("--recovery-session"));
            CHECK(detail.contains(firstName) && detail.contains(secondName));
            const auto location = controller.store()->recoveryDirectory();
            CHECK(location != first && location != second && !controller.store()->hasRecoveryFailures());
            const auto &live = openedFile(controller);
            CHECK(!live.current().draft.text.starts_with("first window")
                  && !live.current().draft.text.starts_with("second window"));
        }
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo, firstName);
            CHECK(controller.store()->recoveryDirectory() == first);
            CHECK(controller.loader()->detail().contains("Recovered app drafts"));
            CHECK(controller.loader()->detail().contains(secondName + " was set aside, not deleted"));
            CHECK(openedFile(controller).current().draft.text.starts_with("first window\n"));
        }
        auto setAside = storeContents(second);
        CHECK(setAside.contains("set-aside") && setAside.value("set-aside").contains(secondName.toUtf8()));
        setAside.remove("set-aside");
        CHECK(setAside == retained);
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            CHECK(controller.store()->recoveryDirectory() == first
                  && !controller.loader()->detail().contains("blocked"));
            CHECK(openedFile(controller).current().draft.text.starts_with("first window\n"));
        }
        // Choosing the set-aside session brings it back and sets the other one aside.
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo, secondName);
            CHECK(controller.store()->recoveryDirectory() == second);
            CHECK(controller.loader()->detail().contains(firstName + " was set aside, not deleted"));
            CHECK(openedFile(controller).current().draft.text.starts_with("second window\n"));
        }
        CHECK(!QFileInfo::exists(second + "/set-aside") && QFileInfo::exists(first + "/set-aside"));
    });
    suite.run("a blocked window lists each session and restores the chosen one without a restart", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString first;
        QString second;
        {
            Application one{test_support::windowConfig()};
            openController(one, repo);
            Application two{test_support::windowConfig()};
            openController(two, repo);
            for (auto *controller : {&one, &two}) {
                const auto &selected = openedFile(*controller);
                const std::string text = controller == &one ? "first window\n" : "second window\n";
                CHECK(
                    awaitAligned(*controller->store(),
                                 controller->store()->editResult(selected.id(), selected.current().id, {{0, 0}, text}))
                    == Transition::Applied);
            }
            first = one.store()->recoveryDirectory();
            second = two.store()->recoveryDirectory();
        }
        const auto firstName = QFileInfo(first).fileName();
        const auto secondName = QFileInfo(second).fileName();
        const auto retained = storeContents(second);
        const auto reload = [](Application &controller, const QString &session) {
            if (!controller.loader()->chooseRecoverySession(session)) return false;
            waitOpened(controller, "the chosen recovery session");
            CHECK(!controller.loader()->opening()
                  && controller.loader()->startupState() == SessionLoader::StartupState::Ready);
            return true;
        };

        // A window with its own edits refuses to switch, so they are never left behind.
        QString third;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            CHECK(controller.loader()->recoveryChoices().size() == 2);
            const auto &live = openedFile(controller);
            CHECK(awaitAligned(*controller.store(),
                               controller.store()->editResult(live.id(), live.current().id, {{0, 0}, "third window\n"}))
                  == Transition::Applied);
            third = controller.store()->recoveryDirectory();
            CHECK(!controller.loader()->chooseRecoverySession(firstName));
            CHECK(controller.feedback()->message().contains("already holds app edits"));
            CHECK(controller.store()->recoveryDirectory() == third
                  && controller.loader()->recoveryChoices().size() == 2);
            CHECK(!QFileInfo::exists(first + "/set-aside") && !QFileInfo::exists(second + "/set-aside"));
        }
        // Retire the third store outside v1 so the remaining checks see two choices again.
        CHECK(QDir().rename(third, repo.executables.path() + "/retired-third"));

        Application controller{test_support::windowConfig()};
        openController(controller, repo);
        const auto choices = controller.loader()->recoveryChoices();
        CHECK(choices.size() == 2);
        QStringList sessions;
        for (const auto &entry : choices) {
            const auto choice = entry.toMap();
            sessions.push_back(choice.value("session").toString());
            CHECK(!choice.value("lastChanged").toString().isEmpty());
            CHECK(choice.value("paths").toStringList() == QStringList{"tracked.txt"});
        }
        sessions.sort();
        QStringList expected{firstName, secondName};
        expected.sort();
        CHECK(sessions == expected);
        const auto own = controller.store()->recoveryDirectory();

        // Unknown sessions and sessions another window holds are refused without changes.
        CHECK(!controller.loader()->chooseRecoverySession(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        CHECK(controller.feedback()->message().contains("not one of the choices"));
        {
            QLockFile held(second + "/session.lock");
            CHECK(held.tryLock(0));
            CHECK(!controller.loader()->chooseRecoverySession(firstName));
            CHECK(controller.feedback()->message().contains(secondName + " is open in another Branch Truce window"));
        }
        CHECK(controller.store()->recoveryDirectory() == own && controller.loader()->recoveryChoices().size() == 2);
        CHECK(!QFileInfo::exists(first + "/set-aside") && !QFileInfo::exists(second + "/set-aside"));

        // The choice reloads through the startup restore: adopted, the other set aside, nothing deleted.
        CHECK(reload(controller, firstName));
        CHECK(controller.store()->recoveryDirectory() == first && controller.loader()->recoveryChoices().isEmpty());
        CHECK(controller.loader()->detail().contains("Recovered app drafts"));
        CHECK(controller.loader()->detail().contains(secondName + " was set aside, not deleted"));
        CHECK(openedFile(controller).current().draft.text.starts_with("first window\n"));
        auto setAside = storeContents(second);
        CHECK(setAside.contains("set-aside"));
        setAside.remove("set-aside");
        CHECK(setAside == retained);
        // The window's earlier store held only launch captures and is deleted.
        CHECK(!QFileInfo::exists(own));
        // Later edits go to the adopted store.
        const auto restoredId = openedFile(controller).id();
        CHECK(awaitAligned(
                  *controller.store(),
                  controller.store()->editResult(restoredId, openedFile(controller).current().id, {{0, 0}, "kept\n"}))
              == Transition::Applied);
        const auto adopted = replayRecovery(storage::RecoveryStore(first));
        const auto kept = std::find_if(adopted.files.begin(), adopted.files.end(),
                                       [&](const auto &file) { return file.id() == restoredId; });
        CHECK(adopted.succeeded && kept != adopted.files.end()
              && kept->current().draft.text.starts_with("kept\nfirst window\n"));
    });
    suite.run("a recovery choice after the operation ended shows no earlier operation", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString first;
        {
            Application one{test_support::windowConfig()};
            openController(one, repo);
            Application two{test_support::windowConfig()};
            openController(two, repo);
            for (auto *controller : {&one, &two}) {
                const auto &selected = openedFile(*controller);
                CHECK(awaitAligned(
                          *controller->store(),
                          controller->store()->editResult(selected.id(), selected.current().id, {{0, 0}, "edit\n"}))
                      == Transition::Applied);
            }
            first = one.store()->recoveryDirectory();
        }
        Application controller{test_support::windowConfig()};
        openController(controller, repo);
        CHECK(controller.loader()->operation() == "Merge");
        repo.git({"merge", "--abort"});
        CHECK(controller.loader()->chooseRecoverySession(QFileInfo(first).fileName()));
        waitOpened(controller, "the reload after the merge ended");
        CHECK(controller.loader()->operation().isEmpty());
        CHECK(controller.loader()->operationDescription().isEmpty());
    });
    suite.run("a recovery choice replays every store on a worker and refuses an unreadable own store later", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString first;
        QString second;
        {
            Application one{test_support::windowConfig()};
            openController(one, repo);
            Application two{test_support::windowConfig()};
            openController(two, repo);
            for (auto *controller : {&one, &two}) {
                const auto &selected = openedFile(*controller);
                const std::string text = controller == &one ? "first window\n" : "second window\n";
                CHECK(
                    awaitAligned(*controller->store(),
                                 controller->store()->editResult(selected.id(), selected.current().id, {{0, 0}, text}))
                    == Transition::Applied);
            }
            first = one.store()->recoveryDirectory();
            second = two.store()->recoveryDirectory();
        }
        const auto firstName = QFileInfo(first).fileName();
        auto *gui = QThread::currentThread();
        using Calls = std::vector<RecordingCorrespondence::Call>;
        // The alignments of the files a replay restored with this text.
        const auto replayed = [](const Calls &calls, const std::function<bool(const std::string &)> &text) {
            Calls matching;
            std::copy_if(calls.begin(), calls.end(), std::back_inserter(matching),
                         [&](const auto &call) { return text(call.result); });
            return matching;
        };
        const auto onWorkers = [&](const Calls &calls) {
            return !calls.empty() && std::none_of(calls.begin(), calls.end(), [&](const auto &call) {
                return call.thread == gui || call.thread == nullptr;
            });
        };
        const auto startingWith = [](const char *prefix) {
            return [prefix](const std::string &text) { return text.starts_with(prefix); };
        };

        const auto alignments = std::make_shared<const RecordingCorrespondence>();
        Application controller{[&] {
            auto config = test_support::windowConfig();
            config.correspondence = alignments;
            return config;
        }()};
        openController(controller, repo);
        CHECK(controller.loader()->recoveryChoices().size() == 2);
        const auto own = controller.store()->recoveryDirectory();
        const auto ownText = openedFile(controller).current().draft.text;
        const auto journal = own + "/journal";
        const auto intact = readFile(journal);
        // A complete line that is no recovery frame makes this window's store unreadable.
        {
            QFile file(journal);
            const QByteArray garbage("not a recovery frame\n");
            CHECK(file.open(QIODevice::Append) && file.write(garbage) == garbage.size());
        }
        // The choice is accepted and opens at once; the worker's read refuses it afterwards.
        alignments->start();
        CHECK(controller.loader()->chooseRecoverySession(firstName));
        CHECK(controller.loader()->opening());
        waitOpened(controller, "the choice refused by its unreadable store");
        auto calls = alignments->stop();
        CHECK(controller.loader()->startupState() == SessionLoader::StartupState::Ready);
        CHECK(controller.feedback()->message().contains("This window's recovery store could not be read"));
        CHECK(controller.store()->recoveryDirectory() == own && controller.loader()->recoveryChoices().size() == 2);
        CHECK(!QFileInfo::exists(first + "/set-aside") && !QFileInfo::exists(second + "/set-aside"));
        // Neither matching store was replayed.
        CHECK(replayed(calls, startingWith("first window\n")).empty());
        CHECK(replayed(calls, startingWith("second window\n")).empty());

        // Readable again, the choice reloads. Every replay on the way, of this
        // window's store and of each matching store, runs on a worker.
        writeFile(journal, intact);
        alignments->start();
        CHECK(controller.loader()->chooseRecoverySession(firstName));
        waitOpened(controller, "the chosen recovery session");
        calls = alignments->stop();
        CHECK(controller.loader()->startupState() == SessionLoader::StartupState::Ready
              && controller.store()->recoveryDirectory() == first);
        CHECK(openedFile(controller).current().draft.text.starts_with("first window\n"));
        const auto ownReplay = replayed(calls, [&](const std::string &text) { return text == ownText; });
        CHECK(std::any_of(ownReplay.begin(), ownReplay.end(), [&](const auto &call) { return call.thread != gui; }));
        CHECK(onWorkers(replayed(calls, startingWith("first window\n"))));
        CHECK(onWorkers(replayed(calls, startingWith("second window\n"))));
    });
    suite.run("a chosen session locked during the reload restores no other session in its place", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString first;
        QString second;
        {
            Application one{test_support::windowConfig()};
            openController(one, repo);
            Application two{test_support::windowConfig()};
            openController(two, repo);
            for (auto *controller : {&one, &two}) {
                const auto &selected = openedFile(*controller);
                const std::string text = controller == &one ? "first window\n" : "second window\n";
                CHECK(
                    awaitAligned(*controller->store(),
                                 controller->store()->editResult(selected.id(), selected.current().id, {{0, 0}, text}))
                    == Transition::Applied);
            }
            first = one.store()->recoveryDirectory();
            second = two.store()->recoveryDirectory();
        }
        const auto firstName = QFileInfo(first).fileName();
        const auto secondName = QFileInfo(second).fileName();
        const auto retainedFirst = storeContents(first);
        const auto retainedSecond = storeContents(second);
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            CHECK(controller.loader()->recoveryChoices().size() == 2);
            const auto own = controller.store()->recoveryDirectory();
            CHECK(controller.loader()->chooseRecoverySession(firstName));
            {
                // Another window takes the chosen session after the choice
                // was checked and before the background restore runs.
                QLockFile held(first + "/session.lock");
                CHECK(held.tryLock(0));
                waitOpened(controller, "the reload after the locked choice");
                CHECK(!controller.loader()->opening()
                      && controller.loader()->startupState() == SessionLoader::StartupState::Ready);
            }
            const auto location = controller.store()->recoveryDirectory();
            CHECK(location != first && location != second && location != own);
            const auto detail = controller.loader()->detail();
            CHECK(detail.contains(firstName + " is active or unavailable"));
            CHECK(!detail.contains(firstName + " does not match"));
            CHECK(detail.contains("restoration is blocked rather than guessing")
                  && !detail.contains("Recovered app drafts"));
            const auto choices = controller.loader()->recoveryChoices();
            CHECK(choices.size() == 1 && choices.front().toMap().value("session").toString() == secondName);
            CHECK(!openedFile(controller).current().draft.text.starts_with("second window"));
            CHECK(!controller.store()->hasRecoveryFailures());
            // Neither edited session is set aside or changed.
            CHECK(storeContents(first) == retainedFirst && storeContents(second) == retainedSecond);
            // The window's earlier store held only launch captures and is deleted.
            CHECK(!QFileInfo::exists(own));
        }
        // Once the other window lets go, the chosen session still restores.
        Application controller{test_support::windowConfig()};
        openController(controller, repo, firstName);
        CHECK(controller.store()->recoveryDirectory() == first);
        CHECK(controller.loader()->detail().contains(secondName + " was set aside, not deleted"));
        CHECK(openedFile(controller).current().draft.text.starts_with("first window\n"));
    });
    suite.run("a recovery-session name that does not match restores no edited session in its place", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        const auto unknown = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString store;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            store = controller.store()->recoveryDirectory();
        }
        // A lone store holding only launch captures has no work to choose
        // between, so it is still adopted.
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo, unknown);
            CHECK(controller.store()->recoveryDirectory() == store);
            CHECK(controller.loader()->detail().contains("Recovery session " + unknown
                                                         + " does not match this operation"));
            CHECK(!controller.loader()->detail().contains("blocked")
                  && controller.loader()->recoveryChoices().isEmpty());
            const auto &selected = openedFile(controller);
            CHECK(
                awaitAligned(*controller.store(),
                             controller.store()->editResult(selected.id(), selected.current().id, {{0, 0}, "edited\n"}))
                == Transition::Applied);
        }
        const auto storeName = QFileInfo(store).fileName();
        const auto retained = storeContents(store);
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo, unknown);
            CHECK(controller.store()->recoveryDirectory() != store);
            CHECK(controller.loader()->detail().contains("Recovery session " + unknown
                                                         + " does not match this operation"));
            CHECK(controller.loader()->detail().contains("restoration is blocked rather than guessing")
                  && !controller.loader()->detail().contains("Recovered app drafts"));
            const auto choices = controller.loader()->recoveryChoices();
            CHECK(choices.size() == 1 && choices.front().toMap().value("session").toString() == storeName);
            CHECK(!openedFile(controller).current().draft.text.starts_with("edited\n"));
            CHECK(storeContents(store) == retained);
        }
        // Without a name, the only edited store is restored as before.
        Application controller{test_support::windowConfig()};
        openController(controller, repo);
        CHECK(controller.store()->recoveryDirectory() == store);
        CHECK(openedFile(controller).current().draft.text.starts_with("edited\n"));
    });
    suite.run("the recovery-session option reaches startup recovery", [&] {
        Repository repo;
        repo.conflict("merge");
        const auto unknown = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto state = repo.launch(application, {"--recovery-session", unknown});
        CHECK(state.value("state") == "ready");
        CHECK(state.value("detail").toString().contains("Recovery session " + unknown
                                                        + " does not match this operation"));
    });
    suite.run("external resolution blocks only its file across reopenings choices history and mutation admission", [] {
        Repository repo;
        repo.conflict("merge");
        writeFile(repo.path("excluded.txt"), "main\n");
        ScopedEnvironment environment(repo.environment);
        Draft recovered;
        QString location;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &selected = openedFile(controller);
            CHECK(awaitAligned(*controller.store(),
                               controller.store()->editResult(selected.id(), 1, {{0, 0}, "acknowledged draft\n"}))
                  == Transition::Applied);
            recovered = selected.current().draft;
            location = controller.store()->recoveryDirectory();
        }
        writeFile(repo.path("tracked.txt"), "resolved outside the app\n");
        repo.git({"add", "--", "tracked.txt"});
        for (int restart = 0; restart < 2; ++restart) {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            auto *model = controller.store();
            const auto &failed = openedFile(controller);
            const auto &healthy = openedFile(controller, "excluded.txt");
            CHECK(model->recoveryDirectory() == location && controller.files()->totalFiles() == 2);
            CHECK(model->hasRecoveryFailure(failed.id()) && !model->hasRecoveryFailure(healthy.id()));
            CHECK(!model->hasPendingRecovery() && !controller.completion()->handoff().clean);
            CHECK(failed.current().draft == recovered && failed.canUndo());
            CHECK(fileRole(*controller.files(), failed, SessionModel::RecoveryStateRole)
                      .value<FileStates::RecoveryState>()
                  == FileStates::RecoveryState::Blocked);
            CHECK(!fileRole(*controller.files(), failed, SessionModel::RecoveryErrorRole).toString().isEmpty());
            CHECK(!fileRole(*controller.files(), failed, SessionModel::PendingInputRole).toBool());
            CHECK(!fileRole(*controller.files(), failed, SessionModel::CanUndoRole).toBool());
            const auto id = QString::fromStdString(failed.id());
            CHECK(controller.selection()->select(id) && !controller.selectedFile()->canEdit());
            CHECK(!commandResult(
                controller,
                controller.commands()->editResult(id, QString::number(failed.current().id), 0, 0, "forbidden")));
            CHECK(!commandResult(controller, controller.commands()->undo(id, QString::number(failed.current().id))));
            CHECK(!commandResult(controller, controller.commands()->redo(id, QString::number(failed.current().id))));
            const auto target = test_support::choiceTarget(failed, SourceSide::Stage2, ChoiceScope::Section);
            CHECK(!controller.choices()->previewChoice(id, QString::number(failed.current().id),
                                                       QString::fromStdString(target.source.contributionId),
                                                       ChoicePreviewFlow::Action::Accept));
            const auto preview =
                previewChoice(failed, failed.current().id, target.source.contributionId, ChoiceAction::Accept);
            CHECK(await(model->applyChoice(failed.id(), preview, true)) == Transition::Blocked);
            CHECK(await(model->replaceDraft(failed.id(), failed.current().id, {"forbidden", {}, 0}))
                  == Transition::Blocked);
            CHECK(awaitAligned(*model, model->editResult(failed.id(), failed.current().id, {{0, 0}, "forbidden"}))
                  == Transition::Blocked);
            CHECK(await(model->undo(failed.id(), failed.current().id)) == Transition::Blocked);
            CHECK(await(model->redo(failed.id(), failed.current().id)) == Transition::Blocked);
            const auto healthyId = QString::fromStdString(healthy.id());
            CHECK(controller.selection()->select(healthyId) && controller.selectedFile()->canEdit());
            CHECK(commandResult(controller,
                                controller.commands()->editResult(healthyId, QString::number(healthy.current().id), 0,
                                                                  0, "healthy manual edit\n")));
            const auto before = healthy.current().draft;
            const auto healthyTarget = test_support::choiceTarget(healthy, SourceSide::Stage2, ChoiceScope::Line);
            CHECK(controller.choices()->previewChoice(healthyId, QString::number(healthy.current().id),
                                                      QString::fromStdString(healthyTarget.source.contributionId),
                                                      ChoicePreviewFlow::Action::Accept));
            CHECK(commandResult(controller, controller.choices()->applyChoice(true)));
            const auto chosen = healthy.current().draft;
            CHECK(chosen != before && chosen.text == before.text);
            CHECK(
                commandResult(controller, controller.commands()->undo(healthyId, QString::number(healthy.current().id)))
                && healthy.current().draft == before);
            CHECK(
                commandResult(controller, controller.commands()->redo(healthyId, QString::number(healthy.current().id)))
                && healthy.current().draft == chosen);
            CHECK(commandResult(controller,
                                controller.commands()->undo(healthyId, QString::number(healthy.current().id))));
            CHECK(fileRole(*controller.files(), healthy, SessionModel::CanUndoRole).toBool());
            const auto repositoryBefore = repo.snapshot();
            checkMutationAdmission(controller, failed, healthy);
            CHECK(repo.snapshot() == repositoryBefore);
            CHECK(model->hasRecoveryFailure(failed.id()) && !model->hasRecoveryFailure(healthy.id()));
            CHECK(!fileRole(*controller.files(), failed, SessionModel::RecoveryErrorRole).toString().isEmpty());
        }
    });
    suite.run("earlier recovered drafts retain their decision vocabulary and direct editing", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        QString location;
        std::optional<FileSession> captured;
        {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            captured = openedFile(controller);
            location = controller.store()->recoveryDirectory();
        }
        CHECK(QDir(location).removeRecursively());
        const std::map<std::string, Decision> vocabulary{{"left", {DecisionValue::Accepted, 1}}, {"right", {}}};
        const FileSession legacy(captured->id(), captured->path(), {captured->current().draft.text, vocabulary, 0},
                                 {.snapshots = *captured->snapshots()});
        CHECK(test_support::leasedStore(location)
                  ->commit(recoveryTransaction(legacy, RecoveryAction::Open, QUuid::createUuid().toString(), {}))
                  .succeeded);
        for (int restart = 0; restart < 2; ++restart) {
            Application controller{test_support::windowConfig()};
            openController(controller, repo);
            const auto &restored = openedFile(controller);
            CHECK(controller.store()->recoveryDirectory() == location);
            CHECK(restored.current().draft.decisions == vocabulary && restored.facts().undecided == 1);
            CHECK(!restored.choices().empty());
            const auto target = test_support::choiceTarget(restored, SourceSide::Stage2, ChoiceScope::Section);
            CHECK(
                previewChoice(restored, restored.current().id, target.source.contributionId, ChoiceAction::Accept).state
                == Transition::Blocked);
            const auto id = QString::fromStdString(restored.id());
            CHECK(controller.selection()->select(id) && controller.selectedFile()->canEdit());
            CHECK(controller.selectedFile()->choices().isEmpty());
            CHECK(!controller.selectedFile()->choiceNotice().isEmpty());
            CHECK(!controller.choices()->previewChoice(id, QString::number(restored.current().id),
                                                       QString::fromStdString(target.source.contributionId),
                                                       ChoicePreviewFlow::Action::Accept));
            CHECK(commandResult(controller,
                                controller.commands()->editResult(id, QString::number(restored.current().id), 0, 0,
                                                                  "legacy manual edit\n")));
            CHECK(restored.current().draft.decisions == vocabulary);
            CHECK(commandResult(controller, controller.commands()->undo(id, QString::number(restored.current().id))));
            CHECK(restored.current().draft.decisions == vocabulary);
        }
    });
    for (const auto &kind : {QString("publication"), QString("staging")}) {
        for (const bool resultPresent : {false, true}) {
            suite.run("interrupted " + kind.toStdString() + " remains uncertain against current disk and index "
                          + std::to_string(resultPresent),
                      [=] {
                          Repository repo;
                          repo.conflict("merge");
                          ScopedEnvironment environment(repo.environment);
                          const QByteArray candidate = "reviewed result awaiting completion\n";
                          QString location;
                          {
                              Application controller{test_support::windowConfig()};
                              openController(controller, repo);
                              auto *model = controller.store();
                              const auto &selected = openedFile(controller);
                              auto edited = selected.current().draft;
                              edited.text = candidate.toStdString();
                              CHECK(await(model->replaceDraft(selected.id(), 1, edited)) == Transition::Applied);
                              const auto revision = selected.current().id;
                              CHECK(await(model->beginMarkResolved(selected.id(), revision, true))
                                    == Transition::Applied);
                              CHECK(await(model->recordCheckpoint(selected.id(), revision, Outcome::Succeeded))
                                    == Transition::Applied);
                              if (kind == "staging") {
                                  writeFile(repo.path("tracked.txt"), candidate);
                                  CHECK(await(model->recordWrite(selected.id(), revision, Outcome::Succeeded))
                                        == Transition::Applied);
                              }
                              location = model->recoveryDirectory();
                              CHECK(await(model->commitRecord(mutationRecoveryTransaction(
                                              selected,
                                              {QUuid::createUuid().toString(),
                                               (kind == "staging" ? MutationKind::Staging : MutationKind::Publication),
                                               {}})))
                                        .commit.succeeded);
                          }
                          if (resultPresent)
                              writeFile(repo.path("tracked.txt"), candidate);
                          else if (kind == "staging") {
                              writeFile(repo.path("tracked.txt"), "different externally staged result\n");
                              repo.git({"add", "--", "tracked.txt"});
                          }
                          const auto repositoryBefore = repo.snapshot();
                          Application controller{test_support::windowConfig()};
                          openController(controller, repo);
                          const auto &restored = openedFile(controller);
                          const auto diagnostic =
                              fileRole(*controller.files(), restored, SessionModel::RecoveryErrorRole).toString();
                          CHECK(diagnostic.contains("Interrupted " + kind)
                                && diagnostic.contains("uncertain", Qt::CaseInsensitive));
                          CHECK(diagnostic.contains(resultPresent ? "result bytes are present"
                                                                  : "cannot establish completion"));
                          CHECK(restored.current().draft.text == candidate.toStdString());
                          CHECK(!restored.facts().resolved && !restored.publication().staged());
                          CHECK(controller.store()->hasRecoveryFailure(restored.id())
                                && !controller.files()->hasPendingRecovery());
                          CHECK(fileRole(*controller.files(), restored, SessionModel::RecoveryStateRole)
                                    .value<FileStates::RecoveryState>()
                                == FileStates::RecoveryState::Blocked);
                          CHECK(awaitAligned(*controller.store(),
                                             controller.store()->editResult(restored.id(), restored.current().id,
                                                                            {{0, 0}, "must inspect"}))
                                == Transition::Blocked);
                          CHECK(repo.snapshot() == repositoryBefore);
                          const auto replay = replayRecovery(storage::RecoveryStore(location));
                          CHECK(replay.succeeded && replay.mutations.size() == 1
                                && !replay.mutations.front().observation.has_value());
                      });
        }
    }
}
