# SPDX-License-Identifier: GPL-3.0-or-later
option(PHOSCHAT_ENABLE_PACKAGING "Configure platform installer generation" OFF)
option(PHOSCHAT_DEB_SHLIBDEPS "Use dpkg-shlibdeps for native Debian packages" ON)
set(PHOSCHAT_TDLIB_RUNTIME "" CACHE FILEPATH "Optional TDLib JSON shared library to redistribute")
set(PHOSCHAT_TDLIB_LICENSE "" CACHE FILEPATH "License text for the redistributed TDLib build")
set(PHOSCHAT_WINDOWS_LICENSE_DIR "" CACHE PATH "License notices for bundled Windows dependencies")

set(phoschat_packaging "${PROJECT_SOURCE_DIR}/packaging")
set(phoschat_app_id "io.phoschat.desktop")

if(UNIX AND NOT APPLE)
    configure_file(
        "${phoschat_packaging}/linux/${phoschat_app_id}.metainfo.xml.in"
        "${PROJECT_BINARY_DIR}/${phoschat_app_id}.metainfo.xml"
        @ONLY
    )
    install(FILES "${phoschat_packaging}/linux/${phoschat_app_id}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${PROJECT_BINARY_DIR}/${phoschat_app_id}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    install(FILES "${phoschat_packaging}/linux/${phoschat_app_id}.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${phoschat_packaging}/THIRD-PARTY-NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/phoschat")
endif()

if(WIN32)
    enable_language(RC)
    configure_file("${phoschat_packaging}/windows/phoschat.rc.in" "${PROJECT_BINARY_DIR}/phoschat.rc" @ONLY)
    target_sources(phoschat PRIVATE "${PROJECT_BINARY_DIR}/phoschat.rc")
endif()

if(PHOSCHAT_TDLIB_RUNTIME)
    if(NOT EXISTS "${PHOSCHAT_TDLIB_RUNTIME}" OR NOT EXISTS "${PHOSCHAT_TDLIB_LICENSE}")
        message(FATAL_ERROR "A packaged TDLib runtime requires both its shared library and license file.")
    endif()
    if(WIN32)
        install(FILES "${PHOSCHAT_TDLIB_RUNTIME}" DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME tdjson.dll)
        install(FILES "${PHOSCHAT_TDLIB_LICENSE}" DESTINATION licenses RENAME TDLib.txt)
    elseif(UNIX AND NOT APPLE)
        # Install the actual library even when the input is a versioned linker symlink.
        get_filename_component(phoschat_tdlib_file "${PHOSCHAT_TDLIB_RUNTIME}" REALPATH)
        install(PROGRAMS "${phoschat_tdlib_file}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/phoschat"
            RENAME libtdjson.so)
        install(FILES "${PHOSCHAT_TDLIB_LICENSE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/phoschat"
            RENAME TDLib.txt)
    endif()
endif()

if(NOT PHOSCHAT_ENABLE_PACKAGING)
    return()
endif()

set(CPACK_PACKAGE_NAME "phoschat")
set(CPACK_PACKAGE_VENDOR "Minibota Labs")
set(CPACK_PACKAGE_CONTACT "PhosChat contributors")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/minibota/phoschat")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Compact IRC, Telegram and Slack desktop client")
set(CPACK_PACKAGE_DESCRIPTION
    "PhosChat is a native multiprotocol chat client with a compact conversation list and separate chat windows.")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PhosChat")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_FILE_NAME "phoschat-${PROJECT_VERSION}-windows-x64-setup")
    set(CPACK_NSIS_DISPLAY_NAME "PhosChat")
    set(CPACK_NSIS_PACKAGE_NAME "PhosChat")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY "${CMAKE_INSTALL_BINDIR}")
    set(CPACK_NSIS_MUI_ICON "${phoschat_packaging}/windows/phoschat.ico")
    set(CPACK_NSIS_MUI_UNIICON "${phoschat_packaging}/windows/phoschat.ico")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "phoschat.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_PACKAGE_EXECUTABLES "phoschat;PhosChat")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${phoschat_packaging}/THIRD-PARTY-NOTICES.md"
        "${phoschat_packaging}/windows/Qt-LGPL-3.0.txt" DESTINATION licenses)
    if(PHOSCHAT_WINDOWS_LICENSE_DIR)
        install(DIRECTORY "${PHOSCHAT_WINDOWS_LICENSE_DIR}/" DESTINATION licenses)
    endif()
    qt_generate_deploy_app_script(TARGET phoschat OUTPUT_SCRIPT phoschat_deploy_script NO_TRANSLATIONS)
    install(SCRIPT "${phoschat_deploy_script}")
elseif(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB;RPM")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "PhosChat contributors")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS "${PHOSCHAT_DEB_SHLIBDEPS}")
    string(CONCAT CPACK_DEBIAN_PACKAGE_DEPENDS
        "libc6 (>= 2.41), libstdc++6 (>= 14), libqt6core6t64 (>= 6.8), libqt6gui6 (>= 6.8), "
        "libqt6widgets6 (>= 6.8), libqt6network6 (>= 6.8), libqt6sql6 (>= 6.8), libqt6dbus6 (>= 6.8), "
        "libqt6sql6-sqlite, qt6-qpa-plugins, ca-certificates")
    if(PHOSCHAT_USE_KEYCHAIN AND TARGET qt6keychain)
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libqt6keychain1")
    endif()
    if(NOT PHOSCHAT_TDLIB_RUNTIME)
        set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libtdjson1.8.38")
    endif()

    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
    set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
    if(PHOSCHAT_TDLIB_RUNTIME)
        string(APPEND CPACK_RPM_PACKAGE_LICENSE " AND BSL-1.0")
        set(CPACK_RPM_SPEC_MORE_DEFINE "%global __provides_exclude_from ^%{_libdir}/phoschat/.*$")
    endif()
    set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
    set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase >= 6.8, qt6-qtbase-gui >= 6.8, ca-certificates")
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/usr/share/applications" "/usr/share/metainfo" "/usr/share/icons"
        "/usr/share/icons/hicolor" "/usr/share/icons/hicolor/scalable"
        "/usr/share/icons/hicolor/scalable/apps" "/usr/share/doc")
else()
    message(FATAL_ERROR "Installer packaging is configured for Windows and Linux.")
endif()

include(CPack)
