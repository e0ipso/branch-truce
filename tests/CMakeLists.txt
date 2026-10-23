add_executable(branch_truce_core_smoke core/BuildSmoke.cpp)
target_link_libraries(branch_truce_core_smoke PRIVATE branch_truce_core branch_truce_build_options)
add_test(NAME core.build-smoke COMMAND branch_truce_core_smoke)
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(NAME lifecycle.cli-guards
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli/SetupGuards.sh")
set_tests_properties(lifecycle.cli-guards PROPERTIES TIMEOUT 30)
if(BRANCH_TRUCE_BUILD_DESKTOP)
    add_test(NAME desktop.startup COMMAND branch-truce --smoke-test)
    set_tests_properties(desktop.startup PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
