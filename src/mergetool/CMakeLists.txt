# The launcher protocol: messages, framing, socket and intake paths, and the session's listener.
# It needs Git's repository identity only, so the launcher and the protocol tests link it alone.
add_library(branch_truce_mergetool_protocol STATIC MergetoolMessages.cpp MergetoolMessages.h MergetoolProtocol.cpp
    MergetoolProtocol.h MergetoolServer.cpp MergetoolServer.h)
target_include_directories(branch_truce_mergetool_protocol PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_mergetool_protocol PUBLIC Qt6::Core Qt6::Network branch_truce_core branch_truce_git
    PRIVATE branch_truce_build_options)
add_library(branch_truce_mergetool OBJECT MergetoolSession.cpp MergetoolSession.h MergetoolLauncher.cpp
    MergetoolLauncher.h)
target_include_directories(branch_truce_mergetool PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_mergetool PUBLIC Qt6::Core Qt6::Network branch_truce_core branch_truce_git
    branch_truce_mergetool_protocol branch_truce_storage branch_truce_session branch_truce_publication
    PRIVATE branch_truce_build_options)
