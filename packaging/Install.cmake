# Only the independent GTK app is installed; never install upstream's Qt app.
install(PROGRAMS
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-picker"
    DESTINATION libexec/keepassxc-gnome)
install(FILES "${CMAKE_BINARY_DIR}/share/locale/ja/LC_MESSAGES/keepassxc-gnome.mo"
    DESTINATION share/locale/ja/LC_MESSAGES)
install(FILES "${CMAKE_SOURCE_DIR}/packaging/org.keepassxc.Gnome.Development.desktop"
    DESTINATION share/applications)
install(FILES "${CMAKE_SOURCE_DIR}/packaging/org.keepassxc.Gnome.Development.svg"
    DESTINATION share/icons/hicolor/scalable/apps)
install(CODE [[
    file(MAKE_DIRECTORY "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin")
    file(CREATE_LINK "../libexec/keepassxc-gnome/keepassxc-gnome"
        "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/keepassxc-gnome" SYMBOLIC)
]])

# Register a distinct MIME package; never replace upstream's files/defaults.
install(FILES "${CMAKE_SOURCE_DIR}/packaging/org.keepassxc.Gnome.Development.xml"
    DESTINATION share/mime/packages)
