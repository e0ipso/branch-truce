add_library(branch_truce_controller STATIC ApplicationController.cpp ApplicationController.h
    ChoicePreviewFlow.cpp ChoicePreviewFlow.h CommandGuard.cpp CommandGuard.h FileNavigator.cpp FileNavigator.h
    NavigationTargets.cpp NavigationTargets.h TextPositions.h SelectedFile.cpp SelectedFile.h
    SessionModel.cpp SessionModel.h RecoveryJournal.cpp RecoveryJournal.h RecoveryRecord.cpp RecoveryRecord.h
    RecoveryRestore.cpp RecoveryRestore.h
    SessionCoordinator.cpp SessionCoordinator.h ResolutionPublisher.cpp ResolutionPublisher.h
    MergetoolProtocol.cpp MergetoolProtocol.h MergetoolSession.cpp MergetoolSession.h
    MergetoolLauncher.cpp MergetoolLauncher.h SyntaxHighlighting.h PaneHighlighter.cpp PaneHighlighter.h
    ResultEditorAdapter.cpp ResultEditorAdapter.h TextChange.cpp TextChange.h
    SessionLoader.cpp SessionLoader.h ConflictCapture.cpp ConflictCapture.h
    WordDifferenceDecorations.cpp WordDifferenceDecorations.h)
qt_add_qml_module(branch_truce_controller
    URI BranchTruce.App VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/BranchTruce/App"
    SOURCES SelectedFile.h)
set_target_properties(branch_truce_controller PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_include_directories(branch_truce_controller PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(branch_truce_controller PUBLIC Qt6::Core Qt6::Network Qt6::Quick branch_truce_core branch_truce_git
    PRIVATE branch_truce_storage branch_truce_build_options)
if(BRANCH_TRUCE_SYNTAX_HIGHLIGHTING)
    target_sources(branch_truce_controller PRIVATE SyntaxHighlighting.cpp)
    target_link_libraries(branch_truce_controller PUBLIC KF6::SyntaxHighlighting)
    # PaneHighlighter.h picks its base class from this definition.
    target_compile_definitions(branch_truce_controller PUBLIC BRANCH_TRUCE_SYNTAX_HIGHLIGHTING)
else()
    target_sources(branch_truce_controller PRIVATE SyntaxHighlightingDisabled.cpp)
endif()
qt_add_executable(branch-truce main.cpp)
# Widgets for QApplication, which the org.kde.desktop style draws through.
target_link_libraries(branch-truce PRIVATE branch_truce_controller Qt6::Widgets
    Qt6::Qml Qt6::Quick Qt6::QuickControls2 branch_truce_build_options)
target_compile_definitions(branch-truce PRIVATE BRANCH_TRUCE_APP_ID="${BRANCH_TRUCE_APP_ID}")
# The window icon main falls back to when the icon theme has no application icon.
# It is compiled in, so installed runs never look at the source tree.
set(_branch_truce_symbol "${PROJECT_SOURCE_DIR}/assets/branding/svg/for-light/branch-truce-symbol.svg")
set_source_files_properties("${_branch_truce_symbol}" PROPERTIES QT_RESOURCE_ALIAS branch-truce-symbol.svg)
qt_add_resources(branch-truce branch_truce_window_icon PREFIX "/branch-truce/icons" FILES "${_branch_truce_symbol}")
install(TARGETS branch-truce RUNTIME DESTINATION "${KDE_INSTALL_BINDIR}")
