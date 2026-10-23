qt_add_qml_module(branch_truce_ui STATIC
    URI BranchTruce VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/BranchTruce"
    QML_FILES Main.qml)
target_link_libraries(branch_truce_ui PRIVATE Qt6::Quick Qt6::QuickControls2 KF6::Kirigami branch_truce_build_options)
target_link_libraries(branch-truce PRIVATE branch_truce_uiplugin)
