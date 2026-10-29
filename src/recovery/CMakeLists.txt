add_library(branch_truce_recovery OBJECT RecoveryJournal.cpp RecoveryJournal.h RecoveryRecord.cpp RecoveryRecord.h
    RecoveryRestore.cpp RecoveryRestore.h)
target_include_directories(branch_truce_recovery PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_recovery PUBLIC Qt6::Core branch_truce_core branch_truce_git branch_truce_storage PRIVATE branch_truce_build_options)
