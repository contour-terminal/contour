set(AppId "org.contourterminal.Contour")

if(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
    set(FREEBSD TRUE)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
    set(OPENBSD TRUE)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(LINUX TRUE)
endif()

option(CONTOUR_PERF_STATS "Enables debug printing some performance stats." OFF)
option(CONTOUR_WAYLAND "Enables Wayland specific code paths." ON)

if(WIN32)
    option(CONTOUR_SIGN_CODE_CERTIFICATE "Path to the certificate file (.pfx) to sign the executable with using signtool. If empty, a self-signed certificate will be generated." "")
    option(CONTOUR_SIGN_CODE_PASSWORD "Password for the certificate file." "")
    set(CONTOUR_SIGN_CODE_PUBLISHER "CN=Contour Terminal Self-Signed" CACHE STRING "Publisher DN for the AppxManifest and Self-Signed Certificate")
endif()

NumberToHex(${PROJECT_VERSION_MAJOR} HEX_MAJOR)
NumberToHex(${PROJECT_VERSION_MINOR} HEX_MINOR)
NumberToHex(${PROJECT_VERSION_PATCH} HEX_PATCH)

# {{{ Setup QT_COMPONENTS
# QT_COMPONENTS is the list of Qt libraries Contour requires for building.
# NB: Widgets is rquired for SystemTrayIcon's fallback implementation
set(QT_COMPONENTS Core Gui Qml Quick QuickControls2 Network Multimedia Widgets OpenGL OpenGLWidgets)
# }}}

message(STATUS "Qt components: ${QT_COMPONENTS}")

find_package(Qt6 COMPONENTS ${QT_COMPONENTS} REQUIRED)

if(CONTOUR_FRONTEND_GUI)
    add_subdirectory(display)
endif()

if(APPLE)
    #set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
    set(CMAKE_BUILD_RPATH "${CMAKE_INSTALL_RPATH}")
    set(CMAKE_MACOSX_RPATH ON)
    #set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
    find_package(Qt6 COMPONENTS Core Quick REQUIRED)
endif()

set(_header_files
    Actions.h
    CaptureScreen.h
    Config.h
    ContourApp.h
)
set(_source_files
    Actions.cpp
    CaptureScreen.cpp
    Config.cpp
    ContourApp.cpp
    main.cpp
)

if(CONTOUR_FRONTEND_GUI)
    list(APPEND _header_files
        Audio.h
        BlurBehind.h
        ContourGuiApp.h
        TerminalSession.h
        TerminalSessionManager.h
        helper.h
    )
    list(APPEND _source_files
        Audio.cpp
        BlurBehind.cpp
        ContourGuiApp.cpp
        TerminalSession.cpp
        TerminalSessionManager.cpp
        helper.cpp
    )
endif()

if(WIN32)
    list(APPEND _source_files
        contour.rc
        windows/TerminalHandoff.cpp
        windows/TerminalHandoff.h
        windows/ITerminalHandoff.h
        windows/AppxManifest.xml
        windows/register_package.ps1
    )
endif()

# Install the AppxManifest and registration script to the bin directory
if(WIN32)
    if(DEFINED ENV{GITHUB_RUN_NUMBER})
        set(CONTOUR_VERSION_TWEAK "$ENV{GITHUB_RUN_NUMBER}")
    else()
        set(CONTOUR_VERSION_TWEAK "0")
    endif()


    configure_file(windows/AppxManifest.xml AppxManifest.xml @ONLY)
    set(AppxManifest "${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml")
    install(FILES "${AppxManifest}" DESTINATION bin)
    install(FILES "windows/register_package.ps1" DESTINATION bin)
endif()

set(_qt_resources resources.qrc)
qt_add_resources(_qt_resources ${_qt_resources})

set(_qml_files
    ui/RequestPermission.qml
    ui/Terminal.qml
    ui/main.qml
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

if (WIN32)
    # Set up the certificate
    if(NOT CONTOUR_SIGN_CODE_CERTIFICATE)
        set(CONTOUR_SIGN_CODE_CERTIFICATE "${CMAKE_CURRENT_BINARY_DIR}/contour_dev.pfx")
        add_custom_command(
            OUTPUT "${CONTOUR_SIGN_CODE_CERTIFICATE}"
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
                "$cert = New-SelfSignedCertificate -Type Custom -Subject '${CONTOUR_SIGN_CODE_PUBLISHER}' -KeyUsage DigitalSignature -FriendlyName 'Contour Code Signing' -CertStoreLocation 'Cert:\\CurrentUser\\My' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') ; $pwd = ConvertTo-SecureString -String '${CONTOUR_SIGN_CODE_PASSWORD}' -Force -AsPlainText ; Export-PfxCertificate -Cert $cert -FilePath '${CONTOUR_SIGN_CODE_CERTIFICATE}' -Password $pwd"
            COMMENT "Generating self-signed code signing certificate: ${CONTOUR_SIGN_CODE_CERTIFICATE}"
            VERBATIM
        )
        add_custom_target(generate_pfx DEPENDS "${CONTOUR_SIGN_CODE_CERTIFICATE}")
        add_dependencies(contour generate_pfx)
    endif()

    find_program(SIGNTOOL_EXECUTABLE signtool PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/x64"
    )

    if(NOT SIGNTOOL_EXECUTABLE)
        message(WARNING "signtool not found. Code signing will be disabled.")
    endif()

    add_custom_command(TARGET contour POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:contour>/Assets
        COMMAND ${CMAKE_COMMAND} -E copy
            "${AppxManifest}"
            "${CMAKE_CURRENT_SOURCE_DIR}/windows/register_package.ps1"
            $<TARGET_FILE_DIR:contour>
        # Copy and rename assets to match the AppxManifest expectations
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-512.png" $<TARGET_FILE_DIR:contour>/Assets/StoreLogo.png
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-128.png" $<TARGET_FILE_DIR:contour>/Assets/Square150x150Logo.png
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo-64.png"  $<TARGET_FILE_DIR:contour>/Assets/Square44x44Logo.png
        COMMENT "Copying AppxManifest.xml, registration scripts, and Assets to output directory"
    )

    if(SIGNTOOL_EXECUTABLE)
        add_custom_command(TARGET contour POST_BUILD
            COMMAND "${SIGNTOOL_EXECUTABLE}" sign /f "${CONTOUR_SIGN_CODE_CERTIFICATE}" /p "${CONTOUR_SIGN_CODE_PASSWORD}" /fd SHA256 /v "$<TARGET_FILE:contour>"
            COMMENT "Signing contour.exe and AppxManifest.xml with ${CONTOUR_SIGN_CODE_CERTIFICATE}"
            VERBATIM
        )
    endif()

    add_custom_target(install_certificate
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "$pwd = ConvertTo-SecureString -String '${CONTOUR_SIGN_CODE_PASSWORD}' -Force -AsPlainText; Import-PfxCertificate -FilePath '${CONTOUR_SIGN_CODE_CERTIFICATE}' -CertStoreLocation 'Cert:\\LocalMachine\\TrustedPeople' -Password $pwd"
        COMMENT "Installing code signing certificate to TrustedPeople store (requires Admin)"
        VERBATIM
    )

    add_custom_target(register_package
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "Add-AppxPackage -Register '$<TARGET_FILE_DIR:contour>/AppxManifest.xml' -ForceApplicationShutdown"
        COMMENT "Registering Contour Appx Package"
        VERBATIM
    )

    add_custom_target(unregister_package
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
            "Get-AppxPackage -Name 'Contour.Terminal' | Remove-AppxPackage -ErrorAction SilentlyContinue"
        COMMENT "Unregistering Contour Appx Package"
        VERBATIM
    )

    if(SIGNTOOL_EXECUTABLE AND CONTOUR_SIGN_CODE_CERTIFICATE)
        install(CODE "
            message(STATUS \"Signing installed executable: \${CMAKE_INSTALL_PREFIX}/bin/contour.exe\")
            execute_process(
                COMMAND \"${SIGNTOOL_EXECUTABLE}\" sign /f \"${CONTOUR_SIGN_CODE_CERTIFICATE}\" /p \"${CONTOUR_SIGN_CODE_PASSWORD}\" /fd SHA256 /v \"\${CMAKE_INSTALL_PREFIX}/bin/contour.exe\"
                RESULT_VARIABLE _sign_result
                OUTPUT_VARIABLE _sign_output
                ERROR_VARIABLE _sign_output
            )
            if(NOT _sign_result EQUAL 0)
                message(WARNING \"Failed to sign installed executable: \${_sign_output}\")
            else()
                message(STATUS \"\${_sign_output}\")
            endif()
        ")
    endif()
endif()

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
    crispy::core
    vtbackend
    Qt6::Core
    ${YAML_CPP_LIBRARIES}
)

# {{{ GUI: Declare Qt build dependencies
if(CONTOUR_FRONTEND_GUI)
    if(OPENBSD)
        find_package(X11 REQUIRED)
    endif()
    target_link_libraries(contour
        PRIVATE
        vtrasterizer
        ContourTerminalDisplay
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
    if(CONTOUR_BUILD_STATIC)
        qt_import_qml_plugins(contour)
    endif()

    if(LINUX AND CONTOUR_WAYLAND)
        find_package(Qt6 COMPONENTS WaylandClient WaylandClientPrivate)
        target_compile_definitions(contour PRIVATE CONTOUR_WAYLAND)
        target_link_libraries(contour PRIVATE Qt6::WaylandClient Qt6::WaylandClientPrivate)
        qt6_generate_wayland_protocol_client_sources(contour FILES "${CMAKE_CURRENT_SOURCE_DIR}/protocols/blur.xml")
        # Explicitly add the generated protocol source code to the target, as the macro sometimes fails to do so.
        target_sources(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/wayland-blur-protocol.c")
        target_sources(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/qwayland-blur.cpp")
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
set(CPACK_WIX_VERSION 4)
# ProductCode must change for every version to trigger a Major Upgrade.
set(CPACK_WIX_PRODUCT_GUID "0E736497-2B72-4117-95E9-54EC6D${HEX_MAJOR}${HEX_MINOR}${HEX_PATCH}")
# UpgradeCode must stay constant to allow upgrades from previous versions.
set(CPACK_WIX_UPGRADE_GUID "0E736497-2B72-4117-95E9-54EC6D000000")
if(WIN32)
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Contour Terminal Emulator ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/wix_patch.xml")
    set(CPACK_WIX_PROPERTY_WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT "Launch Contour")

    # Allow CPack to sign the uninstaller/MSI if we configured signing?
    # CPack WIX Generator has its own signing hooks, but for now we focus on the EXE built by CMake.
endif()
if(APPLE)
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.icns")
endif()
# }}}

# {{{ Qt bundle installation helpers
set(INSTALLED_QT_VERSION 6)

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
            PATTERN "*_test.*" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
            PATTERN "unicode_tablegen.exe" EXCLUDE
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

    set(CPACK_COMPONENTS_ALL_IN_ONE_INSTALL TRUE)
    include(InstallRequiredSystemLibraries)

    # See: https://stackoverflow.com/questions/35612687/cmake-macos-x-bundle-with-bundleutiliies-for-qt-application/48035834#48035834
    set(APP_NAME contour)
    set(App_Contents "${APP_NAME}.app/Contents")
    set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
    set(INSTALL_CMAKE_DIR "${App_Contents}/Resources")



    # Install application icon
    install(FILES "res/images/contour-logo.icns" DESTINATION "${INSTALL_CMAKE_DIR}" RENAME "contour.icns")
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
            COMMAND ${MACDEPLOYQT_EXECUTABLE} \"\${CMAKE_INSTALL_PREFIX}/contour.app\"
                    -always-overwrite
                    -verbose=1
                    -no-strip
                    -qmldir=${CMAKE_CURRENT_SOURCE_DIR}/ui
                    \"-codesign=${CODE_SIGN_CERTIFICATE_ID}\"
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

    set(DEPENDS_EXTRA "${DEPENDS_EXTRA}, libqt6core6, libqt6multimedia6, libqt6opengl6, libqt6openglwidgets6, libqt6widgets6, libssh2-1, qml6-module-qt-labs-platform, qml6-module-qt5compat-graphicaleffects, qml6-module-qtmultimedia, qml6-module-qtqml-workerscript, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-templates, qml6-module-qtquick-window, qt6-qpa-plugins")

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
    install(FILES "contour-run.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/kio/servicemenus" RENAME "${AppId}.RunIn.desktop")
    install(FILES "${AppId}.OpenHere.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/kio/servicemenus")
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



include(CPackComponent)

if(WIN32)
    cpack_add_component(contour_path
        DISPLAY_NAME "Add to PATH"
        DESCRIPTION "Adds the contour executable to the system PATH."
    )
    set(CPACK_COMPONENTS_ALL Unspecified contour contour_path)

    # Workaround for WIX0091: Install a dummy file to ensure component is not empty
    # to avoid "Duplicate Component with identifier 'CM_C_EMPTY_INSTALL_ROOT'"
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt"
        DESTINATION bin
        COMPONENT contour_path
        RENAME LICENSE.contour_path.txt
    )
endif()

if(UNIX)
    # CPackDeb is broken. If no components are used, it does not create a ddeb...
    cpack_add_component(contour
        DISPLAY_NAME "${CPACK_PACKAGE_NAME}"
        REQUIRED
    )
endif()

include(CPack)
