# Desktop integration files. QML is compiled into the executable, so the
# install needs no QML directory; Kirigami and Qt come from system packages.
install(FILES "${BRANCH_TRUCE_APP_ID}.desktop" DESTINATION "${KDE_INSTALL_APPDIR}")
install(FILES "${BRANCH_TRUCE_APP_ID}.metainfo.xml" DESTINATION "${KDE_INSTALL_METAINFODIR}")

# The application icon comes from the branding pack: the detailed 3C symbol as
# the scalable icon, and the simpler small drawing at the sizes it is made for.
set(_branch_truce_hicolor "${PROJECT_SOURCE_DIR}/assets/branding/platform/linux/hicolor")
install(FILES "${_branch_truce_hicolor}/scalable/apps/${BRANCH_TRUCE_APP_ID}.svg"
    DESTINATION "${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps")
foreach(_size 16 22 24 32)
    install(FILES "${_branch_truce_hicolor}/${_size}x${_size}/apps/${BRANCH_TRUCE_APP_ID}.png"
        DESTINATION "${KDE_INSTALL_ICONDIR}/hicolor/${_size}x${_size}/apps")
endforeach()
