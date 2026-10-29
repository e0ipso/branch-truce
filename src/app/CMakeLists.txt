add_library(branch_truce_controller STATIC ApplicationController.cpp ApplicationController.h
    ChoicePreviewFlow.cpp ChoicePreviewFlow.h CommandGuard.cpp CommandGuard.h FileNavigator.cpp FileNavigator.h
    NavigationTargets.cpp NavigationTargets.h TextPositions.h SelectedFile.cpp SelectedFile.h
    SessionModel.cpp SessionModel.h ResultEditorAdapter.cpp ResultEditorAdapter.h TextChange.cpp TextChange.h
    SessionLoader.cpp SessionLoader.h WordDifferenceDecorations.cpp WordDifferenceDecorations.h)
qt_add_qml_module(branch_truce_controller
    URI BranchTruce.App VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/BranchTruce/App"
    SOURCES SelectedFile.h ApplicationController.h SessionModel.h
        ResultEditorAdapter.h SessionLoader.h FileNavigator.h ChoicePreviewFlow.h)
set_target_properties(branch_truce_controller PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_include_directories(branch_truce_controller PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(branch_truce_controller PUBLIC Qt6::Core Qt6::Network Qt6::Qml Qt6::Quick
    branch_truce_core branch_truce_git branch_truce_recovery branch_truce_session
    branch_truce_publication branch_truce_mergetool branch_truce_highlighting
    PRIVATE branch_truce_storage branch_truce_build_options)
# The UI module imports the controller module, and the executable owns both plugins.
add_subdirectory("${PROJECT_SOURCE_DIR}/src/ui" "${PROJECT_BINARY_DIR}/src/ui")
qt_add_executable(branch-truce main.cpp)
# Widgets for QApplication, which the org.kde.desktop style draws through.
target_link_libraries(branch-truce PRIVATE branch_truce_controller branch_truce_uiplugin Qt6::Widgets
    Qt6::Qml Qt6::Quick Qt6::QuickControls2 branch_truce_build_options)
target_compile_definitions(branch-truce PRIVATE BRANCH_TRUCE_APP_ID="${BRANCH_TRUCE_APP_ID}")
# The window icon main falls back to when the icon theme has no application icon.
# It is compiled in, so installed runs never look at the source tree.
set(_branch_truce_symbol "${PROJECT_SOURCE_DIR}/assets/branding/svg/for-light/branch-truce-symbol.svg")
set_source_files_properties("${_branch_truce_symbol}" PROPERTIES QT_RESOURCE_ALIAS branch-truce-symbol.svg)
qt_add_resources(branch-truce branch_truce_window_icon PREFIX "/branch-truce/icons" FILES "${_branch_truce_symbol}")
install(TARGETS branch-truce RUNTIME DESTINATION "${KDE_INSTALL_BINDIR}")
