// Starting the real binary and the controller on a repository: the root and directory arguments,
// the mergetool argument, rebase and cherry-pick routes, offline startup and invalid selections.
#include "StartupRoutes.h"
#include "fixtures/AppSession.h"

using namespace startup_routes;

void startup_routes::addLaunchCases(test_support::Suite &suite, const QString &application)
{
    suite.run("merge startup accepts an explicit root a subdirectory and the current directory", [&] {
        Repository repo;
        repo.conflict("merge");
        CHECK(QDir(repo.root).mkdir("nested"));
        for (const auto &arguments : QList<QStringList>{{repo.root}, {repo.path("nested")}, {}}) {
            const auto state = repo.launch(application, arguments, repo.path("nested"));
            ready(repo, state, "Merge");
            CHECK(state.value("stage2").toString() == "Current checkout");
            CHECK(state.value("stage3").toString() == "Merged commit");
            // Roles and full commit identities stay separate.
            CHECK(state.value("stage2CommitId").toString() == QString::fromLatin1(repo.main));
            CHECK(state.value("stage3CommitId").toString() == QString::fromLatin1(repo.side));
            CHECK(file(state, "excluded.txt").value("support") == "supported");
        }
    });
    suite.run("only a first --mergetool argument selects the launcher", [&] {
        Repository repo;
        repo.conflict("merge");
        CHECK(QDir(repo.root).mkdir("--mergetool"));
        // A repository argument spelled --mergetool opens the window.
        ready(repo, repo.launch(application, {"--", "--mergetool"}), "Merge");
        // Elsewhere the option is refused instead of switching modes.
        QProcess process;
        process.setProcessEnvironment(repo.environment);
        process.setWorkingDirectory(repo.root);
        process.start(application, {"--smoke-test", "--mergetool"});
        CHECK(process.waitForStarted(5000) && process.waitForFinished(20000));
        CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 2);
        CHECK(process.readAllStandardError().contains("--mergetool must be the first argument"));
    });
    suite.run("both rebase startup routes preserve the result and truthful source identities", [&] {
        for (const auto operation : {"rebase-merge", "rebase-apply"}) {
            Repository repo;
            repo.conflict(operation);
            const auto state = repo.launch(application, {repo.root});
            ready(repo, state, "Rebase");
            CHECK(state.value("stage2").toString() == "Current rebased state");
            CHECK(state.value("stage3").toString() == "Commit being replayed");
        }
    });
    suite.run("unsupported cherry-pick keeps the inventory outside app without side labels", [&] {
        Repository repo;
        repo.conflict("cherry-pick");
        const auto state = repo.launch(application, {repo.root});
        CHECK(state.value("state") == "unsupported" && state.value("operation") == "Unsupported operation");
        CHECK(!state.value("detail").toString().isEmpty() && state.value("files").toArray().size() == 2);
        CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
        CHECK(state.value("resolved").toInt() == 0);
        for (const auto &value : state.value("files").toArray()) {
            CHECK(value.toObject().value("support") == "outside-app");
            CHECK(!value.toObject().value("detail").toString().isEmpty());
        }
    });
    suite.run("offline startup uses only local Git inspection with an unreachable remote", [&] {
        Repository repo;
        repo.conflict("merge");
        repo.offlineGuard();
        ready(repo, repo.launch(application, {repo.root}), "Merge");
        CHECK(!repo.offline.networkAttempted());
        const auto trace = repo.offline.calls();
        CHECK(trace.contains("rev-parse\n") && trace.contains("cat-file\n"));
    });
    suite.run("excluded attributes retain the classifier reason beside an accepted manual UTF-8 result", [&] {
        Repository repo(true);
        repo.conflict("merge");
        const auto state = repo.launch(application, {repo.root});
        ready(repo, state, "Merge");
        const auto excluded = file(state, "excluded.txt");
        CHECK(excluded.value("support") == "outside-app" && excluded.value("text").toString().isEmpty());
        CHECK(excluded.value("detail").toString()
              == "Outside app: stage 1 cannot be edited: unsupported or contradictory text/eol/ident attributes");
        CHECK(excluded.value("detail").toString().count(QStringLiteral("Outside app:")) == 1);
        CHECK(state.value("detail").toString().contains("outside app remain outstanding"));
    });
    suite.run("a repository without an operation reports no session and no source identities", [&] {
        Repository repo;
        const auto state = repo.launch(application, {repo.root});
        CHECK(state.value("state") == "no-session" && state.value("files").toArray().isEmpty());
        CHECK(!state.value("detail").toString().isEmpty() && state.value("operation").toString().isEmpty());
        CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
        // the message says no operation runs, not that one has no files left.
        CHECK(state.value("detail").toString().contains("No merge or rebase is in progress"));
        CHECK(state.value("detail").toString().contains("Branch Truce resolves conflicts while a merge or rebase"));
        CHECK(!state.value("detail").toString().contains("no current unmerged files"));
    });
    suite.run("a merge whose conflicts are all resolved says it has no current unmerged files", [&] {
        Repository repo;
        repo.conflict("merge");
        repo.git({"add", "--", "tracked.txt", "excluded.txt"});
        const auto state = repo.launch(application, {repo.root});
        CHECK(state.value("state") == "no-session" && state.value("files").toArray().isEmpty());
        CHECK(state.value("operation") == "Merge");
        CHECK(state.value("detail").toString()
              == "This operation has no current unmerged files. Return to your Git workflow.");
    });
    suite.run("the controller runs the Git it was given, not the one on PATH", [] {
        Repository repo;
        repo.conflict("merge");
        ScopedEnvironment environment(repo.environment);
        // Not on PATH: a wrapper that records its arguments and runs the real Git.
        QTemporaryDir chosen;
        CHECK(chosen.isValid());
        const auto trace = chosen.path() + "/git-argv";
        const auto script = chosen.path() + "/git";
        writeFile(script,
                  ("#!/bin/sh\nprintf '%s\\n' \"$@\" >> " + shellQuote(trace) + "\nexec "
                   + shellQuote(test_support::gitExecutable()) + " \"$@\"\n")
                      .toUtf8(),
                  true);
        const auto path = qgetenv("PATH");
        CHECK(gitOnPath() != script);
        Application controller{test_support::windowConfig(script)};
        openController(controller, repo);
        CHECK(qgetenv("PATH") == path && gitOnPath() != script);
        // Opening the repository inspected it through the wrapper.
        const auto calls = readFile(trace);
        CHECK(calls.contains("rev-parse\n") && calls.contains("cat-file\n"));
        CHECK(openedFile(controller).support() == Support::Supported);
    });
    suite.run("invalid selections expose an error without an empty supported session", [&] {
        Repository repo;
        for (const auto &selection : {repo.executables.path(), repo.path("missing")}) {
            const auto state = repo.launch(application, {selection});
            CHECK(state.value("state") == "error" && state.value("files").toArray().isEmpty());
            CHECK(!state.value("detail").toString().isEmpty());
            CHECK(state.value("stage2").toString().isEmpty() && state.value("stage3").toString().isEmpty());
        }
    });
}
