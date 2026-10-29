add_library(branch_truce_mergetool OBJECT MergetoolProtocol.cpp MergetoolProtocol.h MergetoolSession.cpp MergetoolSession.h
    MergetoolLauncher.cpp MergetoolLauncher.h)
target_include_directories(branch_truce_mergetool PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_mergetool PUBLIC Qt6::Core Qt6::Qml Qt6::Network branch_truce_core branch_truce_git branch_truce_storage PRIVATE branch_truce_build_options)
