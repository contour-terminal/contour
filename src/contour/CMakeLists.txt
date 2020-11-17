include(FilesystemResolver)

find_package(Qt5 COMPONENTS Gui Network Widgets REQUIRED)  # apt install qtbase5-dev libqt5gui5
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)

option(CONTOUR_BLUR_PLATFORM_KWIN "Enables support for blurring transparent background when using KWin (KDE window manager)." OFF)
option(CONTOUR_PERF_STATS "Enables debug printing some performance stats." OFF)
option(CONTOUR_VT_METRICS "Enables collecting and exit-printing some VT usage metrics." OFF)

# {{{ Linux/KDE
# ! apt install extra-cmake-modules libkf5windowsystem-dev
if(CONTOUR_BLUR_PLATFORM_KWIN)
    set(KF5_MIN_VERSION "5.12.0")
    find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})
    include(FeatureSummary)
    find_package(KF5 ${KF5_MIN_VERSION} REQUIRED WindowSystem)
endif()
# }}}

CIncludeMe(contour.yml "${CMAKE_CURRENT_BINARY_DIR}/contour_yaml.h" "default_config_yaml" "contour")

set(contour_SRCS
    Actions.cpp Actions.h
    Config.cpp Config.h
    Controller.cpp Controller.h
    FileChangeWatcher.cpp FileChangeWatcher.h
    LoggingSink.cpp LoggingSink.h
    TerminalWindow.cpp TerminalWindow.h
    TerminalWidget.cpp TerminalWidget.h
    main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/contour_yaml.h"
)

if(WIN32)
    list(APPEND contour_SRCS contour.rc)
endif()

add_executable(contour ${contour_SRCS})
target_include_directories(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(contour PRIVATE
    CONTOUR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CONTOUR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CONTOUR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
set_target_properties(contour PROPERTIES AUTOMOC ON)

if(CONTOUR_PERF_STATS)
    target_compile_definitions(contour PRIVATE CONTOUR_PERF_STATS)
endif()

if(CONTOUR_VT_METRICS)
    target_compile_definitions(contour PRIVATE CONTOUR_VT_METRICS)
endif()

if(WIN32)
    if (NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
        set_target_properties(contour PROPERTIES
            # Tell MSVC to create a GUI application.
            WIN32_EXECUTABLE ON

            # Tell MSVC to use main instead of WinMain for Windows subsystem executables.
            LINK_FLAGS "/ENTRY:mainCRTStartup"
        )
    endif()
elseif(APPLE)
    set(contour_version "0.1.0_pre")
    set_target_properties(contour PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Contour Terminal"
        MACOSX_BUNDLE_INFO_STRING "Contour Terminal Emulator"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.christianparpart.contour"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${contour_version}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${contour_version}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${contour_version}"
        # TODO: MACOSX_BUNDLE_ICON_FILE "contour.icns"
        # TODO: RESOURCE "images/icon.icns"
    )
endif()

target_link_libraries(contour terminal_view yaml-cpp Qt5::Gui Qt5::Widgets Qt5::Network OpenGL::GL)
if(Boost_FILESYSTEM_FOUND)
    target_include_directories(contour PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(contour ${Boost_LIBRARIES})
endif()
if(CONTOUR_BLUR_PLATFORM_KWIN) # TODO: auto-enable on Linux, if KDE dev libs were found.
	target_compile_definitions(contour PRIVATE CONTOUR_BLUR_PLATFORM_KWIN)
    target_link_libraries(contour KF5::WindowSystem)
endif()

if(CONTOUR_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address)
    #add_compile_options(-fsanitize=pointer-compare) # cannot be enabled with -fsanitize=thread
    #add_compile_options(-fsanitize=thread)
    add_compile_options(-fsanitize=undefined)
    target_link_libraries(contour ubsan)
endif()

# ====================================================================================
# INSTALLER
# ====================================================================================

if(NOT(CPACK_GENERATOR))
    if(APPLE)
        set(CPACK_GENERATOR DragNDrop ZIP)
    elseif(WIN32)
        set(CPACK_GENERATOR WIX ZIP)
    endif()
endif()

set(CPACK_PACKAGE_NAME "Contour")
set(CPACK_PACKAGE_VENDOR "https://github.com/christianparpart/contour/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "`contour` is a modern terminal emulator, for everyday use.")
#TODO: set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")

#TODO? set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.ico")
#TODO: set(CPACK_WIX_UI_DIALOG "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-256.png")
#TODO: set(CPACK_WIX_UI_BANNER "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-256.png")

set(CPACK_PACKAGE_EXECUTABLES contour "Contour Terminal Emulator")
set(CPACK_CREATE_DESKTOP_LINKS contour)

if(WIN32)
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.ico")
    set(CPACK_WIX_UPGRADE_GUID "0E736497-2B72-4117-95E9-54EC6DC2432A")
    set(CPACK_WIX_PRODUCT_GUID "DC04A0C6-AAC4-4E1C-835E-8D4223FC5092")

    include(DeployQt)
    windeployqt(contour)
    install(TARGETS contour DESTINATION bin)
    install(
        DIRECTORY "$<TARGET_FILE_DIR:contour>/"
        DESTINATION "bin"
        USE_SOURCE_PERMISSIONS
        FILES_MATCHING
            PATTERN "CMakeFiles" EXCLUDE
            PATTERN "*_autogen" EXCLUDE
            PATTERN "*.h" EXCLUDE
            PATTERN "*.dll"
    )
elseif(APPLE)
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.icns")

    # See: https://stackoverflow.com/questions/35612687/cmake-macos-x-bundle-with-bundleutiliies-for-qt-application/48035834#48035834
    set(APP_NAME contour)
    set(App_Contents "${APP_NAME}.app/Contents")
    set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
    set(INSTALL_CMAKE_DIR "${App_Contents}/Resources")

    # based on code from CMake's QtDialog/CMakeLists.txt
    macro(install_qt5_plugin _qt_plugin_name _qt_plugins_var _prefix)
        get_target_property(_qt_plugin_path "${_qt_plugin_name}" LOCATION)
        if(EXISTS "${_qt_plugin_path}")
            get_filename_component(_qt_plugin_file "${_qt_plugin_path}" NAME)
            get_filename_component(_qt_plugin_type "${_qt_plugin_path}" PATH)
            get_filename_component(_qt_plugin_type "${_qt_plugin_type}" NAME)
            set(_qt_plugin_dest "${_prefix}/PlugIns/${_qt_plugin_type}")
            install(FILES "${_qt_plugin_path}" DESTINATION "${_qt_plugin_dest}")
            set(${_qt_plugins_var} "${${_qt_plugins_var}};\$ENV{DEST_DIR}\${CMAKE_INSTALL_PREFIX}/${_qt_plugin_dest}/${_qt_plugin_file}")
        else()
            message(FATAL_ERROR "QT plugin ${_qt_plugin_name} not found")
        endif()
    endmacro()

    install_qt5_plugin("Qt5::QCocoaIntegrationPlugin" QT_PLUGINS ${App_Contents})

    # ${_qt_plugin_dir} seems empty anyways, but the target bundle/dmg works.
    # file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "[Paths]\nPlugins = ${_qt_plugin_dir}\n")
    # install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" DESTINATION "${INSTALL_CMAKE_DIR}")

    # Install application icon
    install(FILES "res/images/contour-logo.icns" DESTINATION "${INSTALL_CMAKE_DIR}" RENAME "contour.icns")

    #add_custom_target(Docs SOURCES README.md LICENSE.txt)
    #TODO: install(TARGETS Docs ...)

    # Destination paths below are relative to ${CMAKE_INSTALL_PREFIX}
    install(TARGETS ${APP_NAME}
        BUNDLE DESTINATION . COMPONENT Runtime
        RUNTIME DESTINATION "${INSTALL_RUNTIME_DIR}" COMPONENT Runtime
    )

    set(APPS "\${CMAKE_INSTALL_PREFIX}/${APP_NAME}.app")    # Note Mac specific extension .app
    set(DIRS ${CMAKE_BINARY_DIR})                           # Directories to look for dependencies

    # Path used for searching by FIND_XXX(), with appropriate suffixes added
    if(CMAKE_PREFIX_PATH)
        foreach(dir ${CMAKE_PREFIX_PATH})
            list(APPEND DIRS "${dir}/bin" "${dir}/lib")
        endforeach()
    endif()

    # Append Qt's lib folder which is two levels above Qt5Widgets_DIR
    list(APPEND DIRS "${Qt5Widgets_DIR}/../..")

    include(InstallRequiredSystemLibraries)

    message(STATUS "APPS: ${APPS}")
    message(STATUS "DIRS: ${DIRS}")
    message(STATUS "QT_PLUGINS: ${QT_PLUGINS}")

    install(CODE "
        include(BundleUtilities)
        fixup_bundle(\"${APPS}\" \"${QT_PLUGINS}\" \"${DIRS}\")
    ")
else()
    # any other Unix
    include(GNUInstallDirs)
    install(TARGETS contour DESTINATION bin)
    install(FILES "contour.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "res/images/contour-logo-256.png" DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps" RENAME "contour.png")
endif()

include(CPack)
