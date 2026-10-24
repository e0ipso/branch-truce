add_library(branch_truce_controller STATIC ApplicationController.cpp ApplicationController.h
    SessionModel.cpp SessionModel.h SessionRecovery.cpp SessionRecovery.h RecoveryRestore.cpp RecoveryRestore.h
    SessionCoordinator.cpp SessionCoordinator.h ResolutionPublisher.cpp ResolutionPublisher.h)
target_include_directories(branch_truce_controller PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(branch_truce_controller PUBLIC Qt6::Core Qt6::Quick branch_truce_core branch_truce_git
    PRIVATE branch_truce_storage branch_truce_build_options)
qt_add_executable(branch-truce main.cpp)
target_link_libraries(branch-truce PRIVATE branch_truce_controller Qt6::Widgets
    Qt6::Qml Qt6::Quick Qt6::QuickControls2 branch_truce_build_options)
install(TARGETS branch-truce RUNTIME DESTINATION "${KDE_INSTALL_BINDIR}")
