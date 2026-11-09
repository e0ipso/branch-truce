# Desktop integration files. QML is compiled into the executable, so the
# install needs no QML directory; Kirigami and Qt come from system packages.
install(FILES "${BRANCH_TRUCE_APP_ID}.desktop" DESTINATION "${KDE_INSTALL_APPDIR}")
install(FILES "${BRANCH_TRUCE_APP_ID}.metainfo.xml" DESTINATION "${KDE_INSTALL_METAINFODIR}")

# The application icons come from the branding pack: the detailed 3C symbol as
# the scalable icon, and the simpler small drawing at the sizes it is made for.
# installed-files.txt names them, so the package checkers expect the same set.
set(_branch_truce_hicolor "${PROJECT_SOURCE_DIR}/assets/branding/platform/linux/hicolor")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/installed-files.txt" _branch_truce_icons
    REGEX "^share/icons/hicolor/")
foreach(_icon IN LISTS _branch_truce_icons)
    string(REGEX REPLACE "^share/icons/hicolor/" "" _icon "${_icon}")
    cmake_path(GET _icon PARENT_PATH _icon_dir)
    install(FILES "${_branch_truce_hicolor}/${_icon}"
        DESTINATION "${KDE_INSTALL_ICONDIR}/hicolor/${_icon_dir}")
endforeach()
