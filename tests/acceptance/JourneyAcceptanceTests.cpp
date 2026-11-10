// Qt acceptance checks for the PRD's observable acceptance examples. Each journey opens a real Git
// operation in a disposable repository through SessionLoader::openRepository, loads the real
// BranchTruce Main scene offscreen and drives it with mouse clicks and key events. Coverage notes
// and the entry point are in tests/acceptance/README.md.
//
//   JourneyScene    the sandbox, the scene and typed readers of the controller's projections.
//   JourneySteps    clicks, keys, dialogs and the checks journeys share.
//   WindowChecks    fit, wrapping, wheel and placement checks.
//   *Journeys.cpp   the journeys of one group each.
#include "acceptance/JourneySuite.h"
#include "app/Translations.h"
#include "fixtures/GitOnPath.h"
#include "mergetool/MergetoolLauncher.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>

#include <cstring>
#include <iostream>
#include <optional>

// The process runs under tests/with-private-xdg.sh, which gives it a private HOME and XDG
// directories. Each journey's Git, recovery root and socket directory come from its Sandbox
// through Application::Config, and every child process gets its environment from the fixture.
int main(int argc, char **argv)
{
    // Child mode: git mergetool starts this binary as the real launcher. The session in the
    // suite's process already listens, so the launcher never starts another window.
    if (argc == 2 && std::strcmp(argv[1], "--mergetool") == 0) {
        QCoreApplication launcher(argc, argv);
        return branch_truce::runMergetoolLauncher(
            QCoreApplication::arguments(),
            branch_truce::git::GitProgram{gitOnPath(), branch_truce::git::GitEnvironment::fromProcess()},
            {.startWindow = {}});
    }
    const auto arguments = test_support::configure(argc, argv);
    QApplication application(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(application);
    QCoreApplication::setApplicationName("BranchTruceJourneyAcceptance");
    // The suite owns its clipboard and window system; physical desktop input is out of scope.
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        std::cerr << "the journeys run only on the offscreen platform\n";
        return 2;
    }
    journey::JourneySuite suite;
    journey::addReviewJourneys(suite);
    journey::addHandoffJourneys(suite);
    journey::addWindowJourneys(suite);
    journey::addPlatformJourneys(suite);
    // No argument runs every journey; a group name runs one CTest entry's share.
    std::optional<std::string> group;
    if (arguments.size() == 2) group = arguments.at(1);
    if (arguments.size() > 2 || (group && !suite.hasGroup(*group))) {
        std::cerr << "usage: " << arguments.front() << " [--filter TEXT] [--list] [GROUP]\n";
        return 2;
    }
    const auto result = suite.run(group);
    QGuiApplication::clipboard()->clear();
    return result;
}
