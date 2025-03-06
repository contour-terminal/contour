set(AppId "org.contourterminal.Contour")

if(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    set(FREEBSD TRUE)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
    set(OPENBSD TRUE)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(LINUX TRUE)
endif()

option(CONTOUR_PERF_STATS "Enables debug printing some performance stats." OFF)

NumberToHex(${PROJECT_VERSION_MAJOR} HEX_MAJOR)
NumberToHex(${PROJECT_VERSION_MINOR} HEX_MINOR)
NumberToHex(${PROJECT_VERSION_PATCH} HEX_PATCH)

# {{{ Setup QT_COMPONENTS
# QT_COMPONENTS is the list of Qt libraries Contour requires for building.
# NB: Widgets is rquired for SystemTrayIcon's fallback implementation
set(QT_COMPONENTS Core Gui Qml Quick QuickControls2 Network Multimedia Widgets)
if(CONTOUR_QT_VERSION EQUAL 6)
    list(APPEND QT_COMPONENTS OpenGL OpenGLWidgets Core5Compat)
endif()
if(NOT(WIN32) AND NOT(APPLE) AND (CONTOUR_QT_VERSION EQUAL 5))
    list(APPEND QT_COMPONENTS X11Extras)
endif()
# }}}

message(STATUS "Qt components: ${QT_COMPONENTS}")

find_package(Qt${CONTOUR_QT_VERSION} COMPONENTS ${QT_COMPONENTS} REQUIRED)

add_subdirectory(display)

if(APPLE)
    #set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
    set(CMAKE_BUILD_RPATH "${CMAKE_INSTALL_RPATH}")
    set(CMAKE_MACOSX_RPATH ON)
    #set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    find_package(Qt6 COMPONENTS Core Quick REQUIRED)
endif()

set(_source_files
    CaptureScreen.cpp CaptureScreen.h
    main.cpp
)

if(CONTOUR_FRONTEND_GUI)
    list(APPEND _header_files
        Actions.h
        Audio.h
        BlurBehind.h
        Config.h
        ContourApp.h
        ContourGuiApp.h
        TerminalSession.h
        TerminalSessionManager.h
        helper.h
    )
    list(APPEND _source_files
        Actions.cpp
        Audio.cpp
        BlurBehind.cpp
        Config.cpp
        ContourApp.cpp
        ContourGuiApp.cpp
        TerminalSession.cpp
        TerminalSessionManager.cpp
        helper.cpp
    )
endif()

if(WIN32)
    list(APPEND _source_files contour.rc)
endif()

set(_qt_resources resources.qrc)
if(CONTOUR_QT_VERSION EQUAL "5")
    qt5_add_resources(_qt_resources ${_qt_resources})
else()
    qt_add_resources(_qt_resources ${_qt_resources})
endif()

# {{{ configure QML files and their imports
function(DeclareQmlImport name import version)
    set(import_v6_alternative ${ARGN})

    if(CONTOUR_QT_VERSION EQUAL "5")
        set(${name} "${import} ${version}" PARENT_SCOPE)
    elseif("${import_v6_alternative}" STREQUAL "")
        set(${name} "${import}" PARENT_SCOPE)
    else()
        set(${name} "${import_v6_alternative}" PARENT_SCOPE)
    endif()
endfunction()

function(DeclareQML name name_qt5 name_qt6)
    if(CONTOUR_QT_VERSION EQUAL "5")
        set(${name} "${name_qt5}" PARENT_SCOPE)
    else()
        set(${name} "${name_qt6}" PARENT_SCOPE)
    endif()
endfunction()

DeclareQmlImport(qml_import_ContourTerminal "Contour.Terminal" "1.0")
DeclareQmlImport(qml_import_QtQuick "QtQuick" "2.6")
DeclareQmlImport(qml_import_QtQuickControls "QtQuick.Controls" "2.2")
DeclareQmlImport(qml_import_QtQuickLayouts "QtQuick.Layouts" "1.11")
DeclareQmlImport(qml_import_QtQuickWindow "QtQuick.Window" "2.6")
DeclareQmlImport(qml_import_QtQuicklabs "Qt.labs.platform" "1.1")
DeclareQmlImport(qml_import_QtMultimedia "QtMultimedia" "5.12")
DeclareQmlImport(qml_import_QtGraphicalEffects "QtGraphicalEffects" "1.12" "Qt5Compat.GraphicalEffects")
DeclareQML(qml_audio "Audio" "AudioOutput")
DeclareQML(qml_media_player "" "audioOutput: bellAudioOutput")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ui.template/RequestPermission.qml.in ${CMAKE_CURRENT_SOURCE_DIR}/ui/RequestPermission.qml)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ui.template/Terminal.qml.in ${CMAKE_CURRENT_SOURCE_DIR}/ui/Terminal.qml)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ui.template/main.qml.in ${CMAKE_CURRENT_SOURCE_DIR}/ui/main.qml)
# }}}

set(_qml_files
    ui.template/RequestPermission.qml.in
    ui.template/Terminal.qml.in
    ui.template/main.qml.in
)

source_group(Sources FILES ${_source_files})
source_group(Headers FILES ${_header_files})
source_group(Resources FILES ${_qt_resources})
source_group(QML FILES ${_qml_files})

add_executable(contour)
target_sources(contour PRIVATE ${_source_files} ${_header_files} ${_qt_resources} ${_qml_files})

set_target_properties(contour PROPERTIES AUTOMOC ON)
set_target_properties(contour PROPERTIES AUTORCC ON)

target_include_directories(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# {{{ declare compiler definitions
# target_compile_definitions(contour PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:QT_QML_DEBUG>)
target_compile_definitions(contour PRIVATE $<$<CONFIG:Debug>:QT_QML_DEBUG>)
target_compile_definitions(contour PRIVATE $<$<CONFIG:Debug>:QMLJSDEBUGGER>)
target_compile_definitions(contour PRIVATE $<$<CONFIG:Debug>:QT_DECLARATIVE_DEBUG>)
target_compile_definitions(contour PRIVATE
    CONTOUR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CONTOUR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CONTOUR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    CONTOUR_APP_ID="${AppId}"
)

# Disable all deprecated Qt functions prior to Qt 6.0
target_compile_definitions(contour PRIVATE QT_DISABLE_DEPRECATED_BEFORE=0x050F00)

# Create custom target to execute contour
add_custom_target(run COMMAND contour)

if(CONTOUR_PERF_STATS)
    target_compile_definitions(contour PRIVATE CONTOUR_PERF_STATS)
endif()

if(CONTOUR_FRONTEND_GUI)
    target_compile_definitions(contour PRIVATE CONTOUR_FRONTEND_GUI)
endif()

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    target_compile_definitions(contour PRIVATE CONTOUR_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
# }}}

# {{{ platform specific target set_target_properties
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
    set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/Modules" ${CMAKE_MODULE_PATH})
    set_target_properties(contour PROPERTIES
        OUTPUT_NAME "contour"
        MACOSX_RPATH ON
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Contour"
        MACOSX_BUNDLE_INFO_STRING "Contour Terminal Emulator"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${AppId}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${CONTOUR_VERSION_STRING}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${CONTOUR_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${CONTOUR_VERSION}"
        XCODE_ATTRIBUTE_PRODUCT_NAME "Contour Terminal Emulator"
        # TODO: MACOSX_BUNDLE_ICON_FILE "contour.icns"
        # TODO: RESOURCE "images/icon.icns"
    )
endif()
# }}}

if(FREEBSD)
    # FreeBSD does not find yaml-cpp in /usr/local but weirdly everything else.
    target_link_directories(contour PUBLIC "/usr/local/lib")
endif()

if (TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_LIBRARIES yaml-cpp::yaml-cpp)
else()
    set(YAML_CPP_LIBRARIES yaml-cpp)
endif()

target_link_libraries(contour
    PRIVATE
    ContourTerminalDisplay
    crispy::core
    vtbackend
    vtrasterizer
    ${YAML_CPP_LIBRARIES}
)

# {{{ GUI: Declare Qt build dependencies
if(CONTOUR_FRONTEND_GUI)
    if(OPENBSD)
        find_package(X11 REQUIRED)
    endif()
    if(CONTOUR_QT_VERSION EQUAL "6")
        target_link_libraries(contour
            PRIVATE
            Qt6::Core
            Qt6::Core5Compat
            Qt6::Multimedia
            Qt6::Network
            Qt6::OpenGL
            Qt6::Qml
            Qt6::QuickControls2
            Qt6::Widgets
        )

        if(OPENBSD)
            target_link_libraries(contour PRIVATE X11::xcb X11::xcb_cursor)
        elseif(NOT(WIN32) AND NOT(APPLE))
            target_link_libraries(contour PRIVATE xcb)
        endif()
    else()
        target_link_libraries(contour
            PRIVATE
            Qt5::Core
            Qt5::Gui
            Qt5::Multimedia
            Qt5::Network
            Qt5::Widgets
        )
        if(OPENBSD)
            target_link_libraries(contour PRIVATE Qt5::X11Extras X11::xcb)
        elseif(NOT(WIN32) AND NOT(APPLE))
            target_link_libraries(contour PRIVATE Qt5::X11Extras xcb)
        endif()
    endif()
    if(CONTOUR_BUILD_STATIC)
        qt_import_qml_plugins(contour)
    endif()
endif()
# }}}

# {{{ Build terminfo file
if(NOT(WIN32) AND CONTOUR_PACKAGE_TERMINFO)
    set(terminfo_file "contour.terminfo")
    set(terminfo_basedir "${CMAKE_CURRENT_BINARY_DIR}/terminfo")
    set_source_files_properties("${terminfo_file}" PROPERTIES GENERATED TRUE)
    find_program(TIC tic REQUIRED)
    if(${TIC-NOTFOUND})
        message(FATAL_ERROR "Executable tic not found.")
    else()
        message(STATUS "Executable tic found (${TIC}).")
    endif()
    add_custom_command(
        TARGET contour POST_BUILD
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND contour generate terminfo to ${terminfo_file} && ${TIC} -x -o "${terminfo_basedir}" "${terminfo_file}"
        DEPENDS "${terminfo_file}"
        COMMENT "Compiling ${terminfo_file}"
        BYPRODUCTS "${terminfo_file}"
        VERBATIM
    )
endif()
# }}}

# ====================================================================================
# INSTALLER
# ====================================================================================

set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "contour")

# {{{ CPACK variable definitions
if(NOT(CPACK_GENERATOR))
    if(APPLE)
        set(CPACK_GENERATOR DragNDrop)
    elseif(WIN32)
        set(CPACK_GENERATOR WIX ZIP)
    endif()
endif()

set(CPACK_PACKAGE_NAME "Contour")
set(CPACK_PACKAGE_VENDOR "https://github.com/contour-terminal/contour/")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "`contour` is a modern terminal emulator, for everyday use.")
set(CPACK_PACKAGE_CONTACT "Christian Parpart <christian@parpart.family>")
if(WIN32)
    # XXX: WIX does only allow x.x.x.x patterns.
    set(CPACK_PACKAGE_VERSION "${CONTOUR_VERSION}")
else()
    set(CPACK_PACKAGE_VERSION "${CONTOUR_VERSION_STRING}")
endif()
set(CPACK_PACKAGE_EXECUTABLES contour "Contour Terminal Emulator")
set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.ico")
#TODO: set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
#TODO? set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.ico")
#TODO: set(CPACK_WIX_UI_DIALOG "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-256.png")
#TODO: set(CPACK_WIX_UI_BANNER "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-256.png")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE.txt")
set(CPACK_CREATE_DESKTOP_LINKS contour)
set(CPACK_WIX_PRODUCT_GUID "0E736497-2B72-4117-95E9-54EC6D${HEX_MAJOR}${HEX_MINOR}${HEX_PATCH}")
set(CPACK_WIX_UPGRADE_GUID "0E736497-2B72-4117-95E9-54EC6D${HEX_MAJOR}${HEX_MINOR}00")
if(WIN32)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Contour Terminal Emulator ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
endif()
if(APPLE)
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.icns")
endif()
# }}}

# {{{ Qt bundle installation helpers
set(INSTALLED_QT_VERSION ${CONTOUR_QT_VERSION})

function(_qt_get_plugin_name_with_version target out_var)
    string(REGEX REPLACE "^Qt::(.+)" "Qt${INSTALLED_QT_VERSION}::\\1" qt_plugin_with_version "${target}")
    if(TARGET "${qt_plugin_with_version}")
        set("${out_var}" "${qt_plugin_with_version}" PARENT_SCOPE)
    else()
        set("${out_var}" "" PARENT_SCOPE)
    endif()
endfunction()

# if(APPLE)
#    # Required when packaging, and set CMAKE_INSTALL_PREFIX to "/"
#    set(CPACK_SET_DESTDIR TRUE)
#    set(CMAKE_BUNDLE_NAME "contour")
#    set(CMAKE_BUNDLE_LOCATION "/")
#    # make sure CMAKE_INSTALL_PREFIX ends in /
#    set(CMAKE_INSTALL_PREFIX "/${CMAKE_BUNDLE_NAME}.app/Contents")
# endif(APPLE)

macro(bundle_install_qt_plugin _qt_plugin_name _qt_plugins_var)
    # Qt6 provides the plugins as individual packages that need to be found.
    set(_install_prefix "contour.app/Contents/")
    find_package(Qt${INSTALLED_QT_VERSION}${_qt_plugin} QUIET PATHS ${Qt${INSTALLED_QT_VERSION}${_qtComponent}_DIR})
    if(TARGET "${_qt_plugin_name}")
        get_target_property(_qt_plugin_path "${_qt_plugin_name}" LOCATION)
    else()
        _qt_get_plugin_name_with_version("Qt::${_qt_plugin_name}" _qt_plugin_with_version_name)
        if(TARGET "${_qt_plugin_with_version_name}")
            get_target_property(_qt_plugin_path "${_qt_plugin_with_version_name}" LOCATION)
        else()
            message(STATUS "Qt ${_qt_plugin_name}: Ignoring")
            return()
        endif()
    endif()
    if(EXISTS "${_qt_plugin_path}")
        get_filename_component(_qt_plugin_file "${_qt_plugin_path}" NAME)
        get_filename_component(_qt_plugin_type "${_qt_plugin_path}" PATH)
        get_filename_component(_qt_plugin_type "${_qt_plugin_type}" NAME)
        get_filename_component(_qt_plugin_ext "${_qt_plugin_path}" EXT)
        if("${_qt_plugin_ext}" STREQUAL ".dylib")
            set(_qt_plugin_dir "PlugIns")
            set(_qt_plugin_dest "${_install_prefix}${_qt_plugin_dir}/${_qt_plugin_type}")
            message(STATUS "Qt ${_qt_plugin_name}: Installing from ${_qt_plugin_path}")
            install(FILES "${_qt_plugin_path}" DESTINATION "${_qt_plugin_dest}")
            set(${_qt_plugins_var} "${${_qt_plugins_var}};\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_qt_plugin_dest}/${_qt_plugin_file}")
        else()
            message(STATUS "Qt ${_qt_plugin_name}: Ignoring '${_qt_plugin_path}'")
        endif()
    endif()
endmacro()

# _qtComponents  - in-variable name, list of Qt components (like Gui, Qml, Widgets) to install
# _plugins_var   - out-variable name, list variable that will contain all the libraries to maybe carry over
macro(bundle_install_qt_plugins_for_component _qtComponent _plugins_var)
    get_target_property(_qt_module_plugins Qt${INSTALLED_QT_VERSION}::${_qtComponent} ${_plugins_var})
    if(NOT _qt_module_plugins)
        message(STATUS "Qt ${_qtComponent}: no plugins to install")
    else()
        message(STATUS "Qt ${_qtComponent}: collecting ...")
        foreach(_qt_plugin IN LISTS _qt_module_plugins)
            bundle_install_qt_plugin("${_qt_plugin}" "${_plugins_var}")
        endforeach()
    endif()
endmacro()

# get_property(_Qt_Core_LOCATION TARGET Qt${INSTALLED_QT_VERSION}::Core PROPERTY LOCATION)
# get_filename_component(Qt_BIN_DIR "${_Qt_Core_LOCATION}" PATH)
# if(APPLE)
#   get_filename_component(Qt_BIN_DIR "${Qt_BIN_DIR}" PATH)
# endif()
# }}}

if(WIN32)
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
            PATTERN "*"
    )
elseif(APPLE)
    # {{{ NB: This would run macdeployqt after creating contour executable during build stage.
    # This is currently disabled, because it seems like packaging would not work then,
    # but instead, we invoke macdeployqt during cpack stage.
    #
    # include(DeployQt)
    # add_custom_command(
    #     TARGET contour POST_BUILD
    #     COMMENT "Running ${MACDEPLOYQT_EXECUTABLE}"
    #     WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    #     COMMAND ${MACDEPLOYQT_EXECUTABLE} contour.app -always-overwrite -verbose=3 -qmlimport=${CMAKE_CURRENT_SOURCE_DIR}/ui -qmldir=${CMAKE_CURRENT_SOURCE_DIR}/ui
    #     VERBATIM
    # )
    # }}}

    qt_import_qml_plugins(contour)

    include(InstallRequiredSystemLibraries)

    # See: https://stackoverflow.com/questions/35612687/cmake-macos-x-bundle-with-bundleutiliies-for-qt-application/48035834#48035834
    set(APP_NAME contour)
    set(App_Contents "${APP_NAME}.app/Contents")
    set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
    set(INSTALL_CMAKE_DIR "${App_Contents}/Resources")

    # NB: This is mostly based on code from CMake's QtDialog/CMakeLists.txt
    # TODO: Minimize the number of plugins installed to the required ones.
    foreach(_qt_comp IN LISTS QT_COMPONENTS)
        bundle_install_qt_plugins_for_component("${_qt_comp}" QT_PLUGINS)
    endforeach()

    # Install application icon
    install(FILES "res/images/contour-logo.icns" DESTINATION "${INSTALL_CMAKE_DIR}" RENAME "contour.icns")
    install(FILES "res/entitlements.plist" DESTINATION "${App_Contents}")
    install(DIRECTORY "${terminfo_basedir}" DESTINATION "${INSTALL_CMAKE_DIR}")
    install(DIRECTORY "shell-integration" DESTINATION "${INSTALL_CMAKE_DIR}")

    #add_custom_target(Docs SOURCES README.md LICENSE.txt)
    #TODO: install(TARGETS Docs ...)

    # Destination paths below are relative to ${CMAKE_INSTALL_PREFIX}
    install(TARGETS ${APP_NAME}
        BUNDLE DESTINATION . COMPONENT Runtime
        RUNTIME DESTINATION "${INSTALL_RUNTIME_DIR}" COMPONENT Runtime
    )

    # file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "[Paths]\nPlugins = PlugIns\n")
    # install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" DESTINATION "${INSTALL_CMAKE_DIR}")

    # {{{ create vars for fixup_bundle call
    set(BUNDLE_APP "\${CMAKE_INSTALL_PREFIX}/${APP_NAME}.app")        # Note Mac specific extension .app

    # LIBS_SEARCH_DIRS will contain a list of directories to look for dependencies to include into the bundle.
    set(LIBS_SEARCH_DIRS
        "/opt/homebrew/lib"
        "/usr/local/lib"
        "/usr/lib"
        "/lib"
    )

    # Path used for searching by FIND_XXX(), with appropriate suffixes added
    if(CMAKE_PREFIX_PATH)
        foreach(dir ${CMAKE_PREFIX_PATH})
            if (IS_DIRECTORY "${dir}/bin")
                list(APPEND LIBS_SEARCH_DIRS "${dir}/bin")
            endif()
            if (IS_DIRECTORY "${dir}/lib")
                list(APPEND LIBS_SEARCH_DIRS "${dir}/lib")
            endif()
        endforeach()
    endif()

    # some debug printing
    list(SORT QT_PLUGINS)
    list(REMOVE_DUPLICATES QT_PLUGINS)
    foreach(_plugin ${QT_PLUGINS})
        message(STATUS "Qt Plugin: ${_plugin}")
    endforeach()
    foreach(_dir ${LIBS_SEARCH_DIRS})
        message(STATUS "Library Search Dir: ${_dir}")
    endforeach()
    # }}}

    set(CODE_SIGN_CERTIFICATE_ID "-" CACHE STRING "macOS Code signature ID") # TODO: Use proper ID on CI
    include(DeployQt)  # Just to get access to ${MACDEPLOYQT_EXECUTABLE}
    get_filename_component(_macdeployqt_path "${MACDEPLOYQT_EXECUTABLE}" PATH)
    message(STATUS "macdeployqt path: ${_macdeployqt_path}")
    message(STATUS "macdeployqt location: ${MACDEPLOYQT_EXECUTABLE}")

    set(MACDEPLOYQT_QML_WORKAROUND OFF CACHE BOOL "Workaround for macdeployqt not installing Qt QML files.")
    if(MACDEPLOYQT_QML_WORKAROUND)
        # Install Qt QML files
        # This problem is specific to macdeployqt on Github CI, which does not seem to install
        # the Qt QML files, so we do it manually here.
        get_filename_component(Qt6_ROOT_DIR "${Qt6_DIR}/../../.." REALPATH)
        message(STATUS "Using Qt6_ROOT_DIR: ${Qt6_ROOT_DIR}")
        # Qt Qt5Compat QtMultimedia QtQml QtQuick
        foreach(_name IN ITEMS Qt Qt5Compat QtMultimedia QtQml QtQuick)
            get_filename_component(_dir "${Qt6_ROOT_DIR}/share/qt/qml/${_name}" REALPATH)
            message(STATUS "Using _dir: ${_dir}")
            install(DIRECTORY "${_dir}" DESTINATION ${App_Contents}/Resources/qml FILES_MATCHING PATTERN "*")
        endforeach()
    endif()

    install(CODE "
        execute_process(
            WORKING_DIRECTORY \"${_macdeployqt_path}/..\" # is this specific working dir really required? (others believe so)
            COMMAND ${MACDEPLOYQT_EXECUTABLE} \"${CMAKE_CURRENT_BINARY_DIR}/contour.app\"
                    -always-overwrite
                    -verbose=1
                    -no-strip
                    -qmldir=${CMAKE_CURRENT_SOURCE_DIR}/ui
                    \"-codesign=${CODE_SIGN_CERTIFICATE_ID}\"
        )

        # Recursively iterate through all plugins and populate PLUGIN_PATHS to be passed to fixup_bundle.

        message(STATUS \"Globbing into RELATIVE \${CMAKE_CURRENT_BINARY_DIR} for \${CMAKE_SHARED_LIBRARY_SUFFIX} files (\${CMAKE_INSTALL_PREFIX})\")
        file(GLOB_RECURSE PLUGIN_PATHS
            \"\${CMAKE_INSTALL_PREFIX}/contour.app/Contents/PlugIns/*${CMAKE_SHARED_LIBRARY_SUFFIX}\"
            \"\${CMAKE_INSTALL_PREFIX}/contour.app/Contents/Resources/*${CMAKE_SHARED_LIBRARY_SUFFIX}\"
        )
        foreach(_plugin \${PLUGIN_PATHS})
            message(STATUS \"CPack Qt Plugin: \${_plugin}\")
        endforeach()

        include(BundleUtilities)
        fixup_bundle(
            \"${BUNDLE_APP}\"
            \"\${PLUGIN_PATHS}\"
            \"${LIBS_SEARCH_DIRS}\"
        )

        # Post-fix libqcocoa.dylib platform plugin's rpath,
        # The already provided rpath (@loader_path/../../../../lib) does not work
        # in a bundled app.
        # execute_process(
        #     COMMAND install_name_tool
        #         -add_rpath @loader_path/../../Frameworks
        #         \"${BUNDLE_APP}/Contents/PlugIns/platforms/libqcocoa.dylib\"
        # )
        # ^^^
        # Should not be needed, because of fixup_bundle to PLUGIN_PATHS...
        # But for some reason, I cannot get it to work.

        # Ensure code signature
        execute_process(
            WORKING_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}\"
            COMMAND codesign --force --deep --sign \"${CODE_SIGN_CERTIFICATE_ID}\" \"${BUNDLE_APP}\"
        )
    ")
else()
    # any other Unix
    find_program(LSB_RELEASE_EXEC lsb_release)
    execute_process(COMMAND ${LSB_RELEASE_EXEC} -rs
        OUTPUT_VARIABLE LSB_RELEASE_NUMBER
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(DEPENDS_EXTRA ", libyaml-cpp0.7")
    if("${LSB_RELEASE_NUMBER}" STREQUAL "20.04")
        set(DEPENDS_EXTRA ", libyaml-cpp0.6")
    endif()
    if("${LSB_RELEASE_NUMBER}" STREQUAL "24.04")
        set(DEPENDS_EXTRA ", libyaml-cpp0.8")
    endif()

    if(LINUX)
        set(DEPENDS_EXTRA "${DEPENDS_EXTRA}, libutempter0")
    endif()

    if(CONTOUR_QT_VERSION EQUAL "6")
        set(DEPENDS_EXTRA "${DEPENDS_EXTRA}, libqt6core6, libqt6multimedia6, libqt6opengl6, libqt6openglwidgets6, libqt6widgets6, qml6-module-qt-labs-platform, qml6-module-qt5compat-graphicaleffects, qml6-module-qtmultimedia, qml6-module-qtqml-workerscript, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-templates, qml6-module-qtquick-window, qt6-qpa-plugins")
    else()
        set(DEPENDS_EXTRA "${DEPENDS_EXTRA}, libqt5core5a, libqt5gui5, libqt5network5, libqt5multimedia5, libqt5x11extras5") # Qt 5
    endif()

    # Generator: Debian
    # see https://cmake.org/cmake/help/v3.6/module/CPackDeb.html#variable:CPACK_DEBIAN_PACKAGE_DEPENDS
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libfreetype6, libharfbuzz0b${DEPENDS_EXTRA}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/contour-terminal/contour/")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-open")
    set(CPACK_DEBIAN_DEBUGINFO_PACKAGE ON)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    # Override because component-based install would create "contour-contour".
    set(CPACK_DEBIAN_CONTOUR_PACKAGE_NAME "contour")

    set(CPACK_COMPONENTS_ALL "contour")

    include(GNUInstallDirs)
    install(TARGETS contour DESTINATION bin)
    install(FILES "contour.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" RENAME "${AppId}.desktop")
    install(FILES "contour-run.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/kservices5/ServiceMenus" RENAME "${AppId}.RunIn.desktop")
    install(FILES "${AppId}.OpenHere.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/kservices5/ServiceMenus")
    install(FILES "res/images/contour-logo-512.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps" RENAME "${AppId}.png")
    install(FILES "res/images/contour-logo-256.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps" RENAME "${AppId}.png")
    install(FILES "res/images/contour-logo-128.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/128x128/apps" RENAME "${AppId}.png")
    install(FILES "res/images/contour-logo-64.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps" RENAME "${AppId}.png")
    install(FILES "res/images/contour-logo-32.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps" RENAME "${AppId}.png")
    install(FILES "${PROJECT_SOURCE_DIR}/metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo" RENAME "${AppId}.metainfo.xml")
    install(DIRECTORY "shell-integration" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(FILES "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(DIRECTORY "${terminfo_basedir}" DESTINATION "${CMAKE_INSTALL_DATADIR}")
endif()

include(CPack)

if(UNIX)
    # CPackDeb is broken. If no components are used, it does not create a ddeb...
    cpack_add_component(contour
        DISPLAY_NAME "${CPACK_PACKAGE_NAME}"
        REQUIRED
    )
endif()
