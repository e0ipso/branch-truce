add_library(branch_truce_session OBJECT SessionCoordinator.cpp SessionCoordinator.h ConflictCapture.cpp ConflictCapture.h
    SessionStore.cpp SessionStore.h BackgroundAlignment.cpp BackgroundAlignment.h)
target_include_directories(branch_truce_session PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_session PUBLIC Qt6::Core branch_truce_core branch_truce_git branch_truce_storage
    branch_truce_recovery PRIVATE branch_truce_build_options)
