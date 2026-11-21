# Only the independent GTK app is installed; never install upstream's Qt app.
install(PROGRAMS
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-picker"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-browser-host"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-auto-type"
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

set(ime_extension "keepassxc-auto-type-ime@keepassxc-gnome")
install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/gnome-shell/${ime_extension}/metadata.json"
    "${CMAKE_SOURCE_DIR}/packaging/gnome-shell/${ime_extension}/extension.js"
    DESTINATION "share/gnome-shell/extensions/${ime_extension}")
