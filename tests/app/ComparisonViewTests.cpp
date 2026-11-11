// The comparison view in the real Main scene, offscreen. The cases live in one file per feature,
// linked into this executable; this file selects the groups a run executes.
#include "ComparisonCases.h"
#include "app/Translations.h"
#include "fixtures/GuiState.h"
#include <QApplication>

using namespace comparison_scene;

namespace {
int run(SceneSet selection)
{
    test_support::Suite results("ComparisonViewTests");
    Group scenes(results, selection == SceneSet::All || selection == SceneSet::Scenes);
    Group boundary(results, selection == SceneSet::All || selection == SceneSet::Boundary);
    Group display(results, selection == SceneSet::Display);
    const Groups groups{scenes, boundary, display};
    addLayoutCases(groups);
    addEditingCases(groups);
    addTextCases(groups);
    addMarkCases(groups);
    addFoldingCases(groups);
    addNavigationCases(groups);
    addSessionCases(groups);
    addBoundaryCases(groups);
    return results.report();
}
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    // The English catalog supplies the plural forms these checks read.
    branch_truce::installTranslations(application);
    QCoreApplication::setApplicationName("BranchTruceComparisonTests");
    // No argument runs every case; "scenes" and "boundary" select one ctest entry's share.
    // "display" runs the viewport cases on whatever platform Qt was given, such as a real
    // Wayland session.
    const auto arguments = test_support::configure(argc, argv);
    auto selection = SceneSet::All;
    if (arguments.size() == 2 && arguments.at(1) == "scenes")
        selection = SceneSet::Scenes;
    else if (arguments.size() == 2 && arguments.at(1) == "boundary")
        selection = SceneSet::Boundary;
    else if (arguments.size() == 2 && arguments.at(1) == "display")
        selection = SceneSet::Display;
    else if (arguments.size() != 1) {
        std::cerr << "usage: branch_truce_comparison_tests [scenes | boundary | display] [--filter text] [--list]\n";
        return 2;
    }
    // Synthetic offscreen integration owns its own clipboard. Physical desktop
    // input and window-manager behavior remain a separate verification layer.
    // The display cases use neither the clipboard nor real input, and leave the
    // session's clipboard alone.
    if (selection == SceneSet::Display) return run(selection);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        std::cerr << "scenes and boundary run with QT_QPA_PLATFORM=offscreen, not "
                  << QGuiApplication::platformName().toStdString() << '\n';
        return 2;
    }
    const test_support::ScopedClipboard clipboard;
    return run(selection);
}
