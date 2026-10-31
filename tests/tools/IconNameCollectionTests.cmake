# Checks that icon collection retains literal results without treating quoted conditions as icons.
if(NOT SOURCE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "IconNameCollectionTests needs SOURCE_DIR and WORK_DIR")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")
set(qml "${WORK_DIR}/sample.qml")
set(cpp "${WORK_DIR}/model.cpp")
file(WRITE "${qml}" [=[
import QtQuick.Controls
ToolButton {
    property string support: "outside-app"
    property string mode: "review"
    property string iconName: support === "outside-app" ? "text-x-generic" : "dialog-error"
    property var descriptor: ({icon: "binary" !== support ? "document-open" : "dialog-warning"})
    property var nested: ({icon: support == "outside-app" ? (mode === "review" ? "edit-undo" : "edit-redo") : "view-visible"})
    property var constant: ({icon: "outside-app" === "binary" ? "folder-open" : "folder"})
    icon.name: "go-next"
    function fallbackIcon(): string {
        return model.icon || "dialog-information"
    }
}
]=])
file(WRITE "${cpp}" "auto status = Status{.icon = \"document-save\"};\n")
set(output "${WORK_DIR}/icons.txt")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${output}" "-DSOURCE_DIR=${WORK_DIR}"
        "-DQML_FILES=${qml}|${cpp}" -P "${SOURCE_DIR}/tests/app/CollectIconNames.cmake"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Icon collection failed (${status}): ${stdout}${stderr}")
endif()
file(STRINGS "${output}" rows)
set(names "")
foreach(row IN LISTS rows)
    string(FIND "${row}" "\t" separator)
    if(separator LESS 1 OR NOT row MATCHES "\t(sample.qml|model.cpp):[1-9][0-9]*$")
        message(FATAL_ERROR "Icon result lacks a source citation: ${row}")
    endif()
    string(SUBSTRING "${row}" 0 ${separator} name)
    list(APPEND names "${name}")
endforeach()
list(REMOVE_DUPLICATES names)
list(SORT names)
set(expected dialog-error dialog-information dialog-warning document-open document-save
    edit-redo edit-undo folder folder-open go-next text-x-generic view-visible)
list(SORT expected)
if(NOT "${names}" STREQUAL "${expected}")
    message(FATAL_ERROR "Icon results differ. Expected ${expected}; got ${names}")
endif()
