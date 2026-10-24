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

    add_executable(branch_truce_model_tests app/SessionModelTests.cpp)
    target_include_directories(branch_truce_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_model_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.session-model COMMAND branch_truce_model_tests)

    add_executable(branch_truce_coordinator_tests app/SessionCoordinatorTests.cpp)
    target_include_directories(branch_truce_coordinator_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_coordinator_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.session-coordinator COMMAND branch_truce_coordinator_tests)
    set_tests_properties(app.session-coordinator PROPERTIES TIMEOUT 30)

    add_executable(branch_truce_git_tests git/GitAdapterTests.cpp)
    target_include_directories(branch_truce_git_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_git_tests PRIVATE branch_truce_git branch_truce_build_options)
    add_test(NAME git.adapter COMMAND branch_truce_git_tests)
    # Includes the adapter's real 30-second operation deadline.
    set_tests_properties(git.adapter PROPERTIES TIMEOUT 45)

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
