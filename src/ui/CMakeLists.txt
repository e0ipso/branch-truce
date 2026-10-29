qt_add_qml_module(branch_truce_ui STATIC
    URI BranchTruce VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/BranchTruce"
    IMPORTS BranchTruce.App
    IMPORT_PATH "${PROJECT_BINARY_DIR}/src/app"
    QML_FILES
        Main.qml
        PlainLabel.qml
        Pill.qml
        ComparisonView.qml
        ReadingBar.qml
        SourcePane.qml
        ResultPane.qml
        ResultContextMenu.qml
        ConnectorHandle.qml
        LineGutter.qml
        LineBackgrounds.qml
        FoldedRows.qml
        WhitespaceMarkers.qml
        HandoffDialog.qml
        HandoffRow.qml
        WholeFileDialog.qml
        NonConflictingDialog.qml
        ReplacementDialog.qml
        FileList.qml
        FileRow.qml
        MainToolBar.qml
        FileToolBar.qml
        FileNotices.qml
        ChoiceLane.qml
        ChoiceButtons.qml
        SessionFooter.qml
        NamedAction.qml
        MenuAction.qml
        MenuEntry.qml
        EditActions.qml
        StatusChip.qml
        SnapshotPane.qml
        DialogBody.qml
        PreviewBody.qml
        PlainInlineMessage.qml
        PlainPlaceholderMessage.qml
        CenteredPlaceholder.qml
        DiscardStagedDialog.qml
    SOURCES NamedActionMenu.h TextBlocks.cpp TextBlocks.h ComparisonPanes.cpp ComparisonPanes.h PassageSearch.cpp PassageSearch.h)
# Bytecode only: QML compiled to C++ calls Qt's private API
# (QQmlPrivate::AOTCompiledContext), which pins packages to one exact Qt
# release. docs/target-platforms.md records the measured startup cost.
set_target_properties(branch_truce_ui PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_link_libraries(branch_truce_ui PUBLIC branch_truce_controllerplugin)
target_link_libraries(branch_truce_ui PRIVATE Qt6::Quick Qt6::QuickControls2 KF6::Kirigami branch_truce_build_options)
target_link_libraries(branch-truce PRIVATE branch_truce_uiplugin)
