// Startup routes: the real binary and the controller opening conflicted repositories, and what
// a restart restores. The cases live in one file per group, linked into this executable; this file
// selects the group a run executes.
#include "StartupRoutes.h"

using namespace startup_routes;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(application);
    if (argc == 3 && std::string_view(argv[1]) == "--open-edit-and-crash")
        return openEditAndCrash(QString::fromLocal8Bit(argv[2]));
    const auto arguments = test_support::configure(argc, argv);
    const std::map<std::string, void (*)(test_support::Suite &, const QString &)> groups{
        {"publication", addPublicationCases},
        {"recovery", addRecoveryCases},
        {"capture", addCaptureCases},
        {"launch", addLaunchCases}};
    if (arguments.size() < 2 || arguments.size() > 3 || (arguments.size() == 3 && !groups.contains(arguments[2]))) {
        std::cerr
            << "usage: branch_truce_startup_route_tests <branch-truce> [publication | recovery | capture | launch]\n";
        return 2;
    }
    const auto binary = QString::fromStdString(arguments[1]);
    test_support::Suite suite("StartupRouteTests");
    for (const auto &[name, add] : groups)
        if (arguments.size() == 2 || arguments[2] == name) add(suite, binary);
    return suite.report();
}
