add_executable(branch_truce_core_smoke core/BuildSmoke.cpp)
target_compile_definitions(branch_truce_core_smoke PRIVATE
    BRANCH_TRUCE_EXPECTED_VERSION="${BRANCH_TRUCE_RESOLVED_VERSION}")
target_link_libraries(branch_truce_core_smoke PRIVATE branch_truce_core branch_truce_build_options)
add_test(NAME core.build-smoke COMMAND branch_truce_core_smoke)
add_test(NAME core.version-resolution
    COMMAND "${CMAKE_COMMAND}"
        "-DBRANCH_TRUCE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/version-tests"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/core/VersionTests.cmake")
add_test(NAME core.native-version
    COMMAND "${CMAKE_COMMAND}"
        "-DBRANCH_TRUCE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/core/NativeVersionTests.cmake")
add_executable(branch_truce_session_tests core/SessionStateTests.cpp)
target_include_directories(branch_truce_session_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(branch_truce_session_tests PRIVATE branch_truce_core branch_truce_build_options)
# Reject matrix-sized allocations only while the correspondence resource fixture runs.
target_link_options(branch_truce_session_tests PRIVATE "LINKER:--wrap=_Znwm")
add_test(NAME core.session-state COMMAND branch_truce_session_tests)
add_executable(branch_truce_correspondence_fixtures core/CorrespondenceCommandFixtures.cpp)
target_include_directories(branch_truce_correspondence_fixtures PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(branch_truce_correspondence_fixtures PRIVATE branch_truce_core branch_truce_build_options)
add_test(NAME core.correspondence-fixtures COMMAND branch_truce_correspondence_fixtures)
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(NAME lifecycle.cli-guards
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli/SetupGuards.sh")
set_tests_properties(lifecycle.cli-guards PROPERTIES TIMEOUT 30)
# Entries that start the real binary or Qt Quick run through this wrapper. It gives them a private
# HOME and XDG directories, removes those afterwards and fails the run when it created anything
# under the recovery root of the invoking user's real data directory.
set(private_xdg "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/with-private-xdg.sh")
add_test(NAME lifecycle.private-xdg-isolation
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli/PrivateXdgIsolation.sh")
set_tests_properties(lifecycle.private-xdg-isolation PROPERTIES TIMEOUT 30)
# Host side of the .deb build and inspection scripts against a stub container engine (about 1 s).
add_test(NAME packaging.artifact-tooling
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ArtifactTooling.sh")
set_tests_properties(packaging.artifact-tooling PROPERTIES TIMEOUT 30)
# Host side of the installed-artifact validation and the AT-SPI probe against stub
# container, sway and grim commands and a fake Atspi module (about 4 s). It exits 77,
# reported as skipped, when jq or python3 is missing.
add_test(NAME packaging.installed-validation-tooling
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/InstalledValidationTooling.sh")
set_tests_properties(packaging.installed-validation-tooling PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
if(BRANCH_TRUCE_BUILD_DESKTOP)
    add_executable(branch_truce_text_format_tests storage/TextFormatTests.cpp)
    target_include_directories(branch_truce_text_format_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_text_format_tests PRIVATE branch_truce_storage branch_truce_build_options)
    add_test(NAME storage.text-format COMMAND branch_truce_text_format_tests)

    add_executable(branch_truce_recovery_tests storage/RecoveryStoreTests.cpp)
    target_include_directories(branch_truce_recovery_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_recovery_tests PRIVATE branch_truce_storage branch_truce_build_options)
    target_link_options(branch_truce_recovery_tests PRIVATE "LINKER:--wrap=fsync")
    add_test(NAME storage.recovery-store COMMAND branch_truce_recovery_tests)

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
    target_link_options(branch_truce_model_tests PRIVATE "LINKER:--wrap=fsync")
    add_test(NAME app.session-model COMMAND branch_truce_model_tests)

    add_executable(branch_truce_recovery_boundary_tests app/RecoveryBoundaryFaultTests.cpp)
    target_include_directories(branch_truce_recovery_boundary_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_recovery_boundary_tests PRIVATE branch_truce_controller branch_truce_build_options)
    target_link_options(branch_truce_recovery_boundary_tests PRIVATE "LINKER:--wrap=fsync")
    add_test(NAME app.recovery-boundary-faults COMMAND branch_truce_recovery_boundary_tests)
    set_tests_properties(app.recovery-boundary-faults PROPERTIES TIMEOUT 60)

    find_package(Qt6 6.6 REQUIRED COMPONENTS Test)
    add_executable(branch_truce_comparison_tests app/ComparisonViewTests.cpp)
    target_include_directories(branch_truce_comparison_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_comparison_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Widgets Qt6::QuickControls2 Qt6::Network Qt6::Test branch_truce_build_options)
    target_link_options(branch_truce_comparison_tests PRIVATE "LINKER:--wrap=fsync")
    # The executable takes "scenes" or "boundary"; with no argument it runs both groups.
    # "boundary" is the two Backspace and Delete fixtures that edit 262144-byte lines and an
    # 8388608-byte buffer in the real editor. Each timeout is at least twice the slowest of nine runs
    # on the development host, one of them under load from another build: scenes 33.0 to 50.1 s,
    # boundary 17.8 to 23.8 s. The single entry this replaced took 53.7 to 57.5 s against 60 s.
    add_test(NAME app.comparison-view COMMAND branch_truce_comparison_tests scenes)
    add_test(NAME app.comparison-view-boundary COMMAND branch_truce_comparison_tests boundary)
    set_tests_properties(app.comparison-view PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    set_tests_properties(app.comparison-view-boundary PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    if(BRANCH_TRUCE_SYNTAX_HIGHLIGHTING)
        add_executable(branch_truce_syntax_highlighting_tests app/SyntaxHighlightingTests.cpp)
        target_include_directories(branch_truce_syntax_highlighting_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(branch_truce_syntax_highlighting_tests PRIVATE branch_truce_controller
            branch_truce_uiplugin Qt6::Widgets Qt6::QuickControls2 Qt6::Test branch_truce_build_options)
        add_test(NAME app.syntax-highlighting COMMAND branch_truce_syntax_highlighting_tests)
        set_tests_properties(app.syntax-highlighting PROPERTIES TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    endif()

    # PRD observable acceptance examples, driven through the real Main scene and a real Git merge.
    # Entry point and coverage notes: tests/acceptance/README.md.
    add_executable(branch_truce_journey_acceptance_tests acceptance/JourneyAcceptanceTests.cpp)
    target_include_directories(branch_truce_journey_acceptance_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_journey_acceptance_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Widgets Qt6::QuickControls2 Qt6::Test branch_truce_build_options)
    add_test(NAME acceptance.journeys COMMAND branch_truce_journey_acceptance_tests)
    set_tests_properties(acceptance.journeys PROPERTIES TIMEOUT 120 LABELS acceptance
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    add_executable(branch_truce_startup_route_tests app/StartupRouteTests.cpp)
    target_include_directories(branch_truce_startup_route_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_startup_route_tests PRIVATE branch_truce_controller branch_truce_build_options)
    # Fail one initial map in the real load worker; other files use the real constructor.
    set(correspondence_constructor "_ZN12branch_truce17CorrespondenceMapC1EmRKNS_13FileSnapshotsENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt10shared_ptrIKNS_23CorrespondenceAlgorithmEE")
    target_link_options(branch_truce_startup_route_tests PRIVATE "LINKER:--wrap=${correspondence_constructor}")
    target_link_options(branch_truce_startup_route_tests PRIVATE "LINKER:--wrap=fsync")
    add_dependencies(branch_truce_startup_route_tests branch-truce)
    add_test(NAME app.startup-routes COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_startup_route_tests> $<TARGET_FILE:branch-truce>)
    # Real merge fixtures include restart and recovery fault matrices.
    set_tests_properties(app.startup-routes PROPERTIES TIMEOUT 90)

    # Preloaded into the real binary to report its style and icon theme state.
    add_library(branch_truce_appearance_probe MODULE fixtures/AppearanceProbe.cpp)
    target_link_libraries(branch_truce_appearance_probe PRIVATE Qt6::Gui Qt6::QuickControls2 branch_truce_build_options)
    add_executable(branch_truce_startup_appearance_tests app/StartupAppearanceTests.cpp)
    target_include_directories(branch_truce_startup_appearance_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_startup_appearance_tests PRIVATE Qt6::Core branch_truce_build_options)
    add_dependencies(branch_truce_startup_appearance_tests branch-truce branch_truce_appearance_probe)
    add_test(NAME app.startup-appearance COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_startup_appearance_tests>
        $<TARGET_FILE:branch-truce> $<TARGET_FILE:branch_truce_appearance_probe>)
    set_tests_properties(app.startup-appearance PROPERTIES TIMEOUT 90)

    add_executable(branch_truce_resolution_tests app/ResolutionPublisherTests.cpp)
    target_include_directories(branch_truce_resolution_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_resolution_tests PRIVATE branch_truce_controller branch_truce_build_options)
    target_link_options(branch_truce_resolution_tests PRIVATE "LINKER:--wrap=fsync" "LINKER:--wrap=renameat2")
    add_test(NAME app.resolution COMMAND branch_truce_resolution_tests)
    set_tests_properties(app.resolution PROPERTIES TIMEOUT 90)

    add_executable(branch_truce_mergetool_tests app/MergetoolLauncherTests.cpp)
    target_include_directories(branch_truce_mergetool_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_mergetool_tests PRIVATE branch_truce_controller branch_truce_build_options)
    # Its --foreign-peer-launcher child reports another uid for SO_PEERCRED.
    target_link_options(branch_truce_mergetool_tests PRIVATE "LINKER:--wrap=getsockopt")
    add_dependencies(branch_truce_mergetool_tests branch-truce)
    add_test(NAME app.mergetool-launcher COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_mergetool_tests> $<TARGET_FILE:branch-truce>)
    set_tests_properties(app.mergetool-launcher PROPERTIES TIMEOUT 120)

    # Installed /usr/bin/git runs `git mergetool` against the launcher and an in-process session.
    add_executable(branch_truce_mergetool_parent_tests git/MergetoolParentTests.cpp)
    target_include_directories(branch_truce_mergetool_parent_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_mergetool_parent_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_dependencies(branch_truce_mergetool_parent_tests branch-truce)
    add_test(NAME git.mergetool-parent COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_mergetool_parent_tests> $<TARGET_FILE:branch-truce>)
    set_tests_properties(git.mergetool-parent PROPERTIES TIMEOUT 120)

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
    add_test(NAME editor.baseline COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_editor_tests> --baseline)
    add_test(NAME editor.regression COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_editor_tests>)
    set_tests_properties(editor.baseline editor.regression PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    # Gate profile classifies outcomes. The evidence profile publishes timings.
    add_executable(branch_truce_workload_measurements performance/WorkloadMeasurements.cpp)
    target_link_libraries(branch_truce_workload_measurements PRIVATE branch_truce_core
        branch_truce_storage branch_truce_git Qt6::Quick Qt6::QuickControls2 branch_truce_build_options)
    target_compile_definitions(branch_truce_workload_measurements PRIVATE
        "BRANCH_TRUCE_MEASUREMENT_BUILD=\"${CMAKE_BUILD_TYPE}\"")
    qt_add_resources(branch_truce_workload_measurements workload_editor PREFIX "/" BASE "${editor_fixture}"
        FILES "${editor_fixture}/Editor.qml")
    add_test(NAME performance.workloads COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_workload_measurements> --profile gate)
    set_tests_properties(performance.workloads PROPERTIES TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    # Rebuilds a Release copy in a fixture under ${TMPDIR:-/tmp}, installs it, launches only the
    # installed files and removes the fixture (about 100 s on the development host).
    add_test(NAME install.prefix COMMAND ${private_xdg} "${BASH_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/verify-install-prefix.sh"
        --mergetool-tests $<TARGET_FILE:branch_truce_mergetool_tests>)
    # Serial, so its Release build does not slow the wall-clock deadlines of other tests.
    set_tests_properties(install.prefix PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)

    # qmllint exits nonzero on any warning (src/ui/.qmllint.ini), so the generated lint target is the check.
    add_test(NAME ui.qmllint
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target branch_truce_ui_qmllint)
    set_tests_properties(ui.qmllint PROPERTIES TIMEOUT 120 LABELS ui)

    add_test(NAME desktop.startup COMMAND ${private_xdg} $<TARGET_FILE:branch-truce> --smoke-test)
    set_tests_properties(desktop.startup PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
