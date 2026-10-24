add_executable(branch_truce_core_smoke core/BuildSmoke.cpp)
target_link_libraries(branch_truce_core_smoke PRIVATE branch_truce_core branch_truce_build_options)
add_test(NAME core.build-smoke COMMAND branch_truce_core_smoke)
add_executable(branch_truce_session_tests core/SessionStateTests.cpp)
target_include_directories(branch_truce_session_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(branch_truce_session_tests PRIVATE branch_truce_core branch_truce_build_options)
add_test(NAME core.session-state COMMAND branch_truce_session_tests)
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(NAME lifecycle.cli-guards
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli/SetupGuards.sh")
set_tests_properties(lifecycle.cli-guards PROPERTIES TIMEOUT 30)
if(BRANCH_TRUCE_BUILD_DESKTOP)
    add_executable(branch_truce_text_format_tests storage/TextFormatTests.cpp)
    target_include_directories(branch_truce_text_format_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_text_format_tests PRIVATE branch_truce_storage branch_truce_build_options)
    add_test(NAME storage.text-format COMMAND branch_truce_text_format_tests)

    add_executable(branch_truce_destination_tests storage/DestinationWriterTests.cpp)
    target_include_directories(branch_truce_destination_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_destination_tests PRIVATE branch_truce_storage branch_truce_build_options)
    # Faults and exchange races are intercepted only in this test executable.
    target_link_options(branch_truce_destination_tests PRIVATE
        "LINKER:--wrap=write,--wrap=fsync,--wrap=renameat2,--wrap=unlinkat,--wrap=geteuid")
    add_test(NAME storage.destination-writer COMMAND branch_truce_destination_tests)
    set_tests_properties(storage.destination-writer PROPERTIES TIMEOUT 30)

    add_executable(branch_truce_model_tests app/SessionModelTests.cpp)
    target_include_directories(branch_truce_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_model_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.session-model COMMAND branch_truce_model_tests)

    add_executable(branch_truce_startup_route_tests app/StartupRouteTests.cpp)
    target_include_directories(branch_truce_startup_route_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_startup_route_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_dependencies(branch_truce_startup_route_tests branch-truce)
    add_test(NAME app.startup-routes COMMAND branch_truce_startup_route_tests $<TARGET_FILE:branch-truce>)
    set_tests_properties(app.startup-routes PROPERTIES TIMEOUT 45)

    add_executable(branch_truce_coordinator_tests app/SessionCoordinatorTests.cpp)
    target_include_directories(branch_truce_coordinator_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_coordinator_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.session-coordinator COMMAND branch_truce_coordinator_tests)
    set_tests_properties(app.session-coordinator PROPERTIES TIMEOUT 30)

    add_executable(branch_truce_git_tests git/GitAdapterTests.cpp)
    target_include_directories(branch_truce_git_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_git_tests PRIVATE branch_truce_git branch_truce_build_options)
    add_test(NAME git.adapter COMMAND branch_truce_git_tests)
    add_test(NAME git.conflict-classification COMMAND branch_truce_git_tests --classification-only)
    set_tests_properties(git.conflict-classification PROPERTIES TIMEOUT 30)
    # Allow the real 30-second operation deadline plus the Git fixture matrix.
    set_tests_properties(git.adapter PROPERTIES TIMEOUT 90)

    # Keep the accepted standalone editor fixture in the permanent regression suite.
    set(editor_fixture "${PROJECT_SOURCE_DIR}/experiments/editor-integration")
    add_executable(branch_truce_editor_tests "${editor_fixture}/main.cpp")
    target_link_libraries(branch_truce_editor_tests PRIVATE Qt6::Quick Qt6::QuickControls2 branch_truce_build_options)
    qt_add_resources(branch_truce_editor_tests editor_scene PREFIX "/" BASE "${editor_fixture}"
        FILES "${editor_fixture}/Editor.qml")
    add_test(NAME editor.baseline COMMAND branch_truce_editor_tests --baseline)
    add_test(NAME editor.regression COMMAND branch_truce_editor_tests)
    set_tests_properties(editor.baseline editor.regression PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    add_test(NAME desktop.startup COMMAND branch-truce --smoke-test)
    set_tests_properties(desktop.startup PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
