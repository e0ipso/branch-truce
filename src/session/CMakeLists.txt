add_library(branch_truce_session OBJECT SessionCoordinator.cpp SessionCoordinator.h ConflictCapture.cpp ConflictCapture.h)
target_include_directories(branch_truce_session PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_session PUBLIC Qt6::Core Qt6::Qml branch_truce_core branch_truce_git branch_truce_storage PRIVATE branch_truce_build_options)
