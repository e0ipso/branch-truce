# Desktop integration files. QML is compiled into the executable, so the
# install needs no QML directory; Kirigami and Qt come from system packages.
install(FILES "${BRANCH_TRUCE_APP_ID}.desktop" DESTINATION "${KDE_INSTALL_APPDIR}")
install(FILES "${BRANCH_TRUCE_APP_ID}.metainfo.xml" DESTINATION "${KDE_INSTALL_METAINFODIR}")
install(FILES "${BRANCH_TRUCE_APP_ID}.svg" DESTINATION "${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps")
