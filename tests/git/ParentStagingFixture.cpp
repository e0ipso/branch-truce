// The tool `git mergetool` runs in GitAdapterTests' parent staging cases. It stands in for a
// launcher only far enough to exercise the adapter's release preflight; it is not the app's
// launcher.
//
// Usage: parent-staging-fixture SCENARIO MERGED
//
// After the parent has written its temporary sources it installs the scenario's hazard, runs the
// CheckParentStaging preflight on MERGED and records the outcome in marker files. It writes the
// resolution and exits 0 only when the preflight passes.
#include "TestHarness.h"
#include "fixtures/EventLoop.h"
#include "fixtures/Files.h"
#include "fixtures/GitRepository.h"
#include "git/GitAdapter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>

#include <exception>
#include <iostream>

using namespace branch_truce::git;
using test_support::writeFile;

namespace {
void configure(const QString &key, const QString &value)
{
    QProcess process;
    process.start(test_support::gitExecutable(), {"config", key, value});
    CHECK(process.waitForStarted(5000) && process.waitForFinished(5000));
    CHECK(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
}

// The parent ran its own helpers before launching the tool, which the app cannot prevent, so the
// hazards are installed only now.
void installHazard(const QString &scenario)
{
    if (scenario == "filter") {
        writeFile(".gitattributes", "/tracked.txt filter=handoff\n");
        configure("filter.handoff.clean", "touch .handoff-helper-ran; cat");
    } else if (scenario == "hook" || scenario == "custom-hook") {
        const auto hooks = scenario == "hook" ? QString(".git/hooks") : QString("custom hooks");
        CHECK(QDir().mkpath(hooks));
        configure("core.hooksPath", hooks);
        writeFile(hooks + "/post-index-change", "#!/bin/sh\ntouch .handoff-helper-ran\n", true);
    } else if (scenario == "monitor") {
        configure("core.fsmonitor", "touch .handoff-helper-ran");
    } else if (scenario == "promisor") {
        configure("remote.origin.promisor", "true");
    }
}

int resolve(const QString &scenario, const QString &merged)
{
    installHazard(scenario);
    GitAdapter adapter(GitProgram{test_support::gitExecutable(), GitEnvironment::fromProcess()});
    test_support::AsyncCompletion<GitAdapter, Result> completion(adapter, &GitAdapter::finished);
    const Request request{Operation::CheckParentStaging, QDir::currentPath(), QFile::encodeName(merged), {}, 71, 93};
    const auto preflight = completion.run(request);
    CHECK(preflight.request.operation == request.operation && preflight.request.directory == request.directory);
    CHECK(preflight.request.path == request.path && preflight.request.objectId == request.objectId);
    CHECK(preflight.request.sessionGeneration == 71 && preflight.request.inputRevision == 93);
    writeFile(".handoff-preflight", preflight.problem.encoded().toUtf8());
    CHECK(preflight.mutation == Mutation::NotAttempted);
    if (preflight.outcome != Outcome::Success) {
        writeFile(".handoff-rejected", "blocked\n");
        return 1;
    }
    writeFile(".handoff-released", "preflight passed\n");
    writeFile(merged, scenario == "crlf" ? "resolved\r\n" : "resolved\n");
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::cerr << "usage: parent-staging-fixture SCENARIO MERGED\n";
        return 2;
    }
    try {
        return resolve(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
    } catch (const std::exception &error) {
        std::cerr << "parent staging fixture: " << error.what() << '\n';
        return 2;
    }
}
