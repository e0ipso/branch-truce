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
add_executable(branch_truce_word_differences_tests core/WordDifferencesTests.cpp)
target_include_directories(branch_truce_word_differences_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(branch_truce_word_differences_tests PRIVATE branch_truce_core branch_truce_build_options)
add_test(NAME core.word-differences COMMAND branch_truce_word_differences_tests)
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
# Native helper contracts run without container or package-manager mutations.
foreach(native_check IN ITEMS NativeIntegration DebianPreflight FedoraPackaging ArchPackaging)
    add_test(NAME packaging.${native_check}
        COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/${native_check}.sh")
    set_tests_properties(packaging.${native_check} PROPERTIES TIMEOUT 30)
endforeach()

# Host side of the .deb build and inspection scripts against a stub container engine (about 1 s).
add_test(NAME packaging.artifact-tooling
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ArtifactTooling.sh")
set_tests_properties(packaging.artifact-tooling PROPERTIES TIMEOUT 30)
# The CI test stages install every runtime Depends of the package (text parsing, under 1 s).
add_test(NAME packaging.ci-test-stage-depends
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/CiTestStageDepends.sh")
set_tests_properties(packaging.ci-test-stage-depends PROPERTIES TIMEOUT 30)
# The host provisioning list installs every runtime Depends of the package (text parsing, under 1 s).
add_test(NAME packaging.provisioning-depends
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ProvisioningDepends.sh")
set_tests_properties(packaging.provisioning-depends PROPERTIES TIMEOUT 30)
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

    add_executable(branch_truce_text_change_tests app/TextChangeTests.cpp)
    target_include_directories(branch_truce_text_change_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_text_change_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.text-change COMMAND branch_truce_text_change_tests)

    add_executable(branch_truce_navigation_targets_tests app/NavigationTargetsTests.cpp)
    target_include_directories(branch_truce_navigation_targets_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_navigation_targets_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.navigation-targets COMMAND branch_truce_navigation_targets_tests)

    add_executable(branch_truce_conflict_capture_tests app/ConflictCaptureTests.cpp)
    target_include_directories(branch_truce_conflict_capture_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_conflict_capture_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_test(NAME app.conflict-capture COMMAND branch_truce_conflict_capture_tests)

    add_executable(branch_truce_recovery_boundary_tests app/RecoveryBoundaryFaultTests.cpp)
    target_include_directories(branch_truce_recovery_boundary_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_recovery_boundary_tests PRIVATE branch_truce_controller branch_truce_build_options)
    target_link_options(branch_truce_recovery_boundary_tests PRIVATE "LINKER:--wrap=fsync")
    add_test(NAME app.recovery-boundary-faults COMMAND branch_truce_recovery_boundary_tests)
    set_tests_properties(app.recovery-boundary-faults PROPERTIES TIMEOUT 60)

    add_executable(branch_truce_recovery_record_tests app/RecoveryRecordTests.cpp)
    target_include_directories(branch_truce_recovery_record_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_recovery_record_tests PRIVATE branch_truce_controller branch_truce_storage
        branch_truce_build_options)
    target_compile_definitions(branch_truce_recovery_record_tests PRIVATE
        BRANCH_TRUCE_RECOVERY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/recovery")
    add_test(NAME app.recovery-record COMMAND branch_truce_recovery_record_tests)

    find_package(Qt6 6.6 REQUIRED COMPONENTS Test QuickTest)
    add_executable(branch_truce_comparison_tests app/ComparisonViewTests.cpp)
    target_include_directories(branch_truce_comparison_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_comparison_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Widgets Qt6::QuickControls2 Qt6::Network Qt6::Test Qt6::QuickTest branch_truce_build_options)
    target_link_options(branch_truce_comparison_tests PRIVATE "LINKER:--wrap=fsync")
    # The executable takes "scenes" or "boundary"; with no argument it runs both groups.
    # "display" reruns the result viewport cases on a real display and is not a ctest entry:
    #   QT_QPA_PLATFORM=wayland tests/with-private-xdg.sh build/desktop/tests/branch_truce_comparison_tests display
    # with WAYLAND_DISPLAY naming the compositor socket by absolute path, because the wrapper
    # replaces XDG_RUNTIME_DIR. "boundary" is the two Backspace and Delete fixtures that edit 262144-byte lines and an
    # 8388608-byte buffer in the real editor. Each timeout is at least twice the slowest measured run on
    # the development host. Boundary took 17.8 to 23.8 s over nine runs, one of them under load from
    # another build. Scenes run under org.kde.desktop, the style the app selects at startup; a run under
    # the default Basic style would add no coverage, as the app never selects Basic. Five such runs at
    # load averages of 2 to 12 took 180 to 213 s, against 84 s under Basic, so scenes get 450 s.
    add_test(NAME app.comparison-view COMMAND branch_truce_comparison_tests scenes)
    add_test(NAME app.comparison-view-boundary COMMAND branch_truce_comparison_tests boundary)
    set_tests_properties(app.comparison-view PROPERTIES TIMEOUT 450
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop")
    set_tests_properties(app.comparison-view-boundary PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    add_executable(branch_truce_comparison_panes_tests app/ComparisonPanesTests.cpp)
    target_include_directories(branch_truce_comparison_panes_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_comparison_panes_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Quick branch_truce_build_options)
    add_test(NAME app.comparison-panes COMMAND branch_truce_comparison_panes_tests)
    set_tests_properties(app.comparison-panes PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    add_executable(branch_truce_passage_search_tests app/PassageSearchTests.cpp)
    target_include_directories(branch_truce_passage_search_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_passage_search_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Quick Qt6::Test branch_truce_build_options)
    add_test(NAME app.passage-search COMMAND branch_truce_passage_search_tests)
    set_tests_properties(app.passage-search PROPERTIES TIMEOUT 60)

    add_executable(branch_truce_text_blocks_tests app/TextBlocksTests.cpp)
    target_include_directories(branch_truce_text_blocks_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_text_blocks_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Quick Qt6::Test branch_truce_build_options)
    add_test(NAME app.text-blocks COMMAND branch_truce_text_blocks_tests)
    set_tests_properties(app.text-blocks PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    # Built either way: with highlighting OFF it checks the plain highlighters.
    add_executable(branch_truce_syntax_highlighting_tests app/SyntaxHighlightingTests.cpp)
    target_include_directories(branch_truce_syntax_highlighting_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_syntax_highlighting_tests PRIVATE branch_truce_controller
        branch_truce_uiplugin Qt6::Widgets Qt6::QuickControls2 Qt6::Test branch_truce_build_options)
    add_test(NAME app.syntax-highlighting COMMAND branch_truce_syntax_highlighting_tests)
    set_tests_properties(app.syntax-highlighting PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    # The real Main scene under the app's style and icon themes: every icon name src/ui uses
    # resolves, the sidebar and panes paint the View background of a dark and a light KDE colour
    # scheme, and every coloured line has a gutter glyph. Each exits 77, reported as skipped,
    # when this host lacks what it checks against. app/ThemeTests.cpp says how to make each fail.
    # The icon names are collected at build time from src/ui and from the model, which names
    # each file's status icon.
    file(GLOB theme_icon_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/ui/*.qml")
    list(APPEND theme_icon_sources "${PROJECT_SOURCE_DIR}/src/app/SessionModel.cpp")
    list(JOIN theme_icon_sources "|" theme_icon_files)
    set(theme_icon_list "${CMAKE_CURRENT_BINARY_DIR}/theme-icon-names.txt")
    add_custom_command(OUTPUT "${theme_icon_list}"
        COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${theme_icon_list}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DQML_FILES=${theme_icon_files}" -P "${CMAKE_CURRENT_SOURCE_DIR}/app/CollectIconNames.cmake"
        DEPENDS ${theme_icon_sources} "${CMAKE_CURRENT_SOURCE_DIR}/app/CollectIconNames.cmake"
        COMMENT "Collecting the icon names in src/ui and the session model"
        VERBATIM)
    add_custom_target(branch_truce_theme_icon_names DEPENDS "${theme_icon_list}")
    add_executable(branch_truce_theme_tests app/ThemeTests.cpp)
    target_include_directories(branch_truce_theme_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_theme_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Widgets Qt6::QuickControls2 Qt6::Test branch_truce_build_options)
    add_dependencies(branch_truce_theme_tests branch_truce_theme_icon_names)
    add_test(NAME ui.theme-icons COMMAND branch_truce_theme_tests icons "${theme_icon_list}")
    add_test(NAME ui.theme-roles-dark COMMAND branch_truce_theme_tests roles BreezeDark)
    add_test(NAME ui.theme-roles-light COMMAND branch_truce_theme_tests roles BreezeLight)
    add_test(NAME ui.theme-cues COMMAND branch_truce_theme_tests cues)
    set_tests_properties(ui.theme-icons ui.theme-roles-dark ui.theme-roles-light ui.theme-cues PROPERTIES
        TIMEOUT 60 LABELS ui SKIP_RETURN_CODE 77
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    # Typing and wheel scrolling in the real panes with a highlighted 200-line C++ file. It fails
    # when either median CPU time exceeds its limit in docs/resource-limits.md, and skips when the
    # build has BRANCH_TRUCE_SYNTAX_HIGHLIGHTING off. The timeout is twice the slowest of twelve
    # runs on the development host: 50.6 to 52.3 s alone or three at once, 81.0 to 90.7 s six at once.
    add_test(NAME performance.highlighted-editor COMMAND branch_truce_theme_tests cost)
    # CPU medians must not compete with other CTest processes.
    set_tests_properties(performance.highlighted-editor PROPERTIES TIMEOUT 200 RUN_SERIAL TRUE LABELS "ui;performance"
        SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    # PRD observable acceptance examples, driven through the real Main scene and a real Git merge.
    # Entry point and coverage notes: tests/acceptance/README.md.
    add_executable(branch_truce_journey_acceptance_tests acceptance/JourneyAcceptanceTests.cpp)
    target_include_directories(branch_truce_journey_acceptance_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_journey_acceptance_tests PRIVATE branch_truce_controller branch_truce_uiplugin
        Qt6::Widgets Qt6::QuickControls2 Qt6::Test branch_truce_build_options)
    # The executable takes "review", "handoff" or "window"; with no argument it runs every journey.
    # "review" is the six journeys on the three panes, choices, editing, search and Save. "handoff" is the six
    # on the handoff, discarding back to the staged result, startup recovery and mergetool. "window" is the five
    # on window fit, first undecided region, hand edits, line choices inside one conflict and the close guard. Each timeout is at least twice the slowest
    # of four runs on the development host, one of them with the three groups running together: review 23.5 to
    # 26.6 s (36.5 s together), handoff 21.9 to 23.1 s (39.2 s), window 15.5 to 15.9 s (26.8 s). Those window runs
    # predate the line-choice journey; with it, two runs alone under load from other builds took 18.4 and 18.7 s.
    # The single entry these replaced took 85 to 108 s against 120 s. The window journeys run under org.kde.desktop,
    # the style the app selects at startup, as the window fit is what users see; the app never selects the default
    # Basic style, so a Basic run would add no coverage. Under it two runs at load averages of 3 and 4 took 31.1 and
    # 38.1 s, against 29.7 s under Basic.
    add_test(NAME acceptance.journeys-review COMMAND branch_truce_journey_acceptance_tests review)
    add_test(NAME acceptance.journeys-handoff COMMAND branch_truce_journey_acceptance_tests handoff)
    add_test(NAME acceptance.journeys-window COMMAND branch_truce_journey_acceptance_tests window)
    foreach(journey_group review handoff window)
        set_tests_properties(acceptance.journeys-${journey_group} PROPERTIES TIMEOUT 120 LABELS acceptance
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    endforeach()
    set_tests_properties(acceptance.journeys-window PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop")

    add_executable(branch_truce_startup_route_tests app/StartupRouteTests.cpp)
    target_include_directories(branch_truce_startup_route_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_startup_route_tests PRIVATE branch_truce_controller branch_truce_build_options)
    # Fail one initial map in the real load worker; other files use the real constructor.
    set(correspondence_constructor "_ZN12branch_truce17CorrespondenceMapC1EmRKNS_13FileSnapshotsENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt10shared_ptrIKNS_23CorrespondenceAlgorithmEE")
    target_link_options(branch_truce_startup_route_tests PRIVATE "LINKER:--wrap=${correspondence_constructor}")
    target_link_options(branch_truce_startup_route_tests PRIVATE "LINKER:--wrap=fsync")
    # Record the thread of every recovery store read, which every replay starts with.
    target_link_options(branch_truce_startup_route_tests PRIVATE
        "LINKER:--wrap=_ZNK12branch_truce7storage13RecoveryStore4readEv")
    add_dependencies(branch_truce_startup_route_tests branch-truce)
    add_test(NAME app.startup-routes COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_startup_route_tests> $<TARGET_FILE:branch-truce>)
    # Real merge fixtures include restart and recovery fault matrices. Alone it takes 71 to 79 s.
    # With 24 busy loops on 8 cores (load averages of 25 to 36) 14 runs on the host and in the
    # Tuxedo image took 287 to 463 s, so it gets 600 s: the longest run plus 25 %, rounded up.
    set_tests_properties(app.startup-routes PROPERTIES TIMEOUT 600)

    # Preloaded into the real binary to report its style and icon theme state.
    add_library(branch_truce_appearance_probe MODULE fixtures/AppearanceProbe.cpp)
    target_link_libraries(branch_truce_appearance_probe PRIVATE Qt6::Gui Qt6::QuickControls2 branch_truce_build_options)
    add_executable(branch_truce_startup_appearance_tests app/StartupAppearanceTests.cpp)
    target_include_directories(branch_truce_startup_appearance_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_startup_appearance_tests PRIVATE Qt6::Core branch_truce_build_options)
    target_compile_definitions(branch_truce_startup_appearance_tests PRIVATE BRANCH_TRUCE_APP_ID="${BRANCH_TRUCE_APP_ID}")
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
    # Alone it takes about 40 s. At load averages of 25 to 36 on 8 cores 14 runs on the host and in
    # the Tuxedo image took 97 to 174 s, so it gets 240 s: the longest run plus 25 %, rounded up.
    set_tests_properties(app.mergetool-launcher PROPERTIES TIMEOUT 240)

    # Installed /usr/bin/git runs `git mergetool` against the launcher and an in-process session.
    add_executable(branch_truce_mergetool_parent_tests git/MergetoolParentTests.cpp)
    target_include_directories(branch_truce_mergetool_parent_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_mergetool_parent_tests PRIVATE branch_truce_controller branch_truce_build_options)
    add_dependencies(branch_truce_mergetool_parent_tests branch-truce)
    add_test(NAME git.mergetool-parent COMMAND ${private_xdg} $<TARGET_FILE:branch_truce_mergetool_parent_tests> $<TARGET_FILE:branch-truce>)
    # Alone it takes about 35 s. At load averages of 25 to 36 on 8 cores 14 runs on the host and in
    # the Tuxedo image took 107 to 218 s, so it gets 300 s: the longest run plus 25 %, rounded up.
    set_tests_properties(git.mergetool-parent PROPERTIES TIMEOUT 300)

    add_executable(branch_truce_coordinator_tests app/SessionCoordinatorTests.cpp)
    target_include_directories(branch_truce_coordinator_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(branch_truce_coordinator_tests PRIVATE branch_truce_controller Qt6::Test branch_truce_build_options)
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

    # Keep the accepted editor fixture, which links no product target, in the permanent regression suite.
    set(editor_fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/editor")
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
    # Wall-clock budgets must not compete with other CTest processes.
    set_tests_properties(performance.workloads PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS performance
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
    # This invokes the build tool and may regenerate shared QML metadata. Keep it
    # exclusive of all tests reading build outputs, including the icon-name list.
    set_tests_properties(ui.qmllint PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS ui)

    add_test(NAME desktop.startup COMMAND ${private_xdg} $<TARGET_FILE:branch-truce> --smoke-test)
    set_tests_properties(desktop.startup PROPERTIES TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
