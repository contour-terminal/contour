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

NumberToHex(${PROJECT_VERSION_MAJOR} HEX_MAJOR)
NumberToHex(${PROJECT_VERSION_MINOR} HEX_MINOR)
NumberToHex(${PROJECT_VERSION_PATCH} HEX_PATCH)

# {{{ Setup QT_COMPONENTS
# QT_COMPONENTS is the list of Qt libraries Contour requires for building.
# NB: Widgets is rquired for SystemTrayIcon's fallback implementation
# NB: GuiPrivate provides rhi/qrhi.h (RHI resource model) used by the display lib and the screenshot
#     readback test. It must be requested here explicitly: relying on a sibling subdirectory's
#     find_package to have populated the Qt6::GuiPrivate target is order-dependent and fails on some
#     Qt packagings (e.g. Homebrew Qt on macOS), where contour_gui_test then cannot link it.
set(QT_COMPONENTS Core Gui GuiPrivate Qml Quick QuickControls2 Network Multimedia Widgets DBus)
# }}}

message(STATUS "Qt components: ${QT_COMPONENTS}")

find_package(Qt6 COMPONENTS ${QT_COMPONENTS} REQUIRED)

# {{{ Optional features excluded from this build
# The Qt version CI builds and validates against. Building against an older Qt is supported, but a
# feature needing a newer Qt — or an optional Qt module that is not installed — is then silently left
# out of the binary. Silently is the problem: a packager discovers it from a bug report rather than
# from their build log.
#
# So a gate does not print its own warning. It records itself here, and the single report at the
# bottom of this file names every excluded feature at once. Adding a gated feature is adding one
# contour_note_excluded_feature() call next to the gate; the reporting needs no edit.
set(CONTOUR_QT_VALIDATED_VERSION "6.11")

# Records that FEATURE is not built, and why.
#
# @param FEATURE Human-readable feature name, as it should read in the warning.
# @param REASON  Why it is excluded, e.g. "requires Qt 6.11" or "Qt6::TextToSpeech not found".
function(contour_note_excluded_feature FEATURE REASON)
    set_property(GLOBAL APPEND PROPERTY CONTOUR_EXCLUDED_FEATURES "${FEATURE} (${REASON})")
endfunction()
# }}}

# {{{ Optional: Qt's TextToSpeech, for reading a selection aloud
# Optional rather than required, per AGENT.md's no-new-dependencies rule: a feature one action wide
# must not make every packager carry another Qt module. When it is missing the feature compiles out and
# the menu row disappears -- and the report at the bottom of this file names it, so that is visible in
# the build log rather than discovered from a bug report.
find_package(Qt6 COMPONENTS TextToSpeech QUIET)
if(Qt6TextToSpeech_FOUND)
    set(CONTOUR_WITH_TTS ON)
else()
    contour_note_excluded_feature("Read selection aloud" "Qt6::TextToSpeech not found")
endif()
# }}}

add_subdirectory(display)

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
    AsciiText.h
    AtomicFileWrite.h
    CaptureScreen.h
    Command.h
    CommandCatalog.h
    CommandHistory.h
    CommandHistoryStore.h
    Config.h
    ContextMenu.h
    ContextMenuModel.h
    ContextMenuTable.h
    TitleBarContextMenu.h
    ContourApp.h
    FuzzyFilter.h
    GuiConfigStore.h
    HyperlinkTooltip.h
    SpeechSynthesizer.h
    ModifierNames.h
    Shortcut.h
    TabBarMode.h
    TabLabel.h
)
# Sources are split into a reusable object library (contour_core) and the thin executable entry
# point (main.cpp). The object library carries every translation unit of the frontend so that both
# the `contour` executable AND the `contour_gui_test` target can link the exact same objects — this
# is what lets the GUI tests construct real TerminalSession/ContourGuiApp objects instead of only
# Qt-free surrogates. An OBJECT library (not STATIC) is used deliberately: it shares the compiled
# objects without a static archive's symbol-stripping, which would otherwise drop AUTOMOC/AUTORCC
# registration objects whose symbols are only referenced reflectively at runtime.
set(_core_source_files
    Actions.cpp
    AtomicFileWrite.cpp
    CaptureScreen.cpp
    Command.cpp
    CommandCatalog.cpp
    CommandHistory.cpp
    CommandHistoryStore.cpp
    Config.cpp
    ContextMenu.cpp
    ContextMenuModel.cpp
    TitleBarContextMenu.cpp
    ContourApp.cpp
    FuzzyFilter.cpp
    GuiConfigStore.cpp
    HyperlinkTooltip.cpp
    SpeechSynthesizer.cpp
    Shortcut.cpp
    TabLabel.cpp
)
# The executable entry point lives outside the object library so the test can supply its own main
# (Catch2's) without a duplicate-symbol clash.
set(_main_source_files
    main.cpp
)

list(APPEND _header_files
    Audio.h
    AudioNote.h
    BlurBehind.h
    CatImageArgs.h
    CommandPaletteModel.h
    ContourGuiApp.h
    ExitCode.h
    ExternalLauncher.h
    FreeDesktopNotifier.h
    LayoutStore.h
    NotificationRouter.h
    PaneProxy.h
    QtExternalLauncher.h
    SessionFactory.h
    SettingsController.h
    TerminalSession.h
    TerminalSessionManager.h
    helper.h
    remote/NativeController.h
    remote/RemoteController.h
    remote/ReactorThread.h
    remote/RemoteLayout.h
    remote/RoutingSessionFactory.h
    remote/TmuxController.h
)
list(APPEND _core_source_files
    Audio.cpp
    BlurBehind.cpp
    CommandPaletteModel.cpp
    ContourGuiApp.cpp
    FreeDesktopNotifier.cpp
    PaneProxy.cpp
    QtExternalLauncher.cpp
    LayoutBuilder.cpp
    LayoutStore.cpp
    SessionFactory.cpp
    SettingsController.cpp
    TerminalSession.cpp
    display/TerminalAccessible.cpp
    TerminalSessionManager.cpp
    WindowController.cpp
    helper.cpp
    remote/NativeController.cpp
    remote/RemoteController.cpp
    remote/RemoteLayout.cpp
    remote/TmuxController.cpp
)

if(WIN32)
    list(APPEND _main_source_files contour.rc)
endif()

set(_qt_resources resources.qrc)
qt_add_resources(_qt_resources ${_qt_resources})

set(_qml_files
    ui/BellSound.qml
    ui/RequestPermission.qml
    ui/main.qml
)

source_group(Sources FILES ${_core_source_files} ${_main_source_files})
source_group(Headers FILES ${_header_files})
source_group(Resources FILES ${_qt_resources})
source_group(QML FILES ${_qml_files})

# {{{ contour_core: the reusable object library (everything but main.cpp)
add_library(contour_core OBJECT)
target_sources(contour_core PRIVATE ${_core_source_files} ${_header_files} ${_qt_resources} ${_qml_files})

set_target_properties(contour_core PROPERTIES AUTOMOC ON)
set_target_properties(contour_core PROPERTIES AUTORCC ON)

# Both the binary dir (for generated headers) and the parent src dir (so "#include <contour/...>"
# resolves) must be PUBLIC so consumers of the object library inherit them.
target_include_directories(contour_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
# }}}

add_executable(contour)
target_sources(contour PRIVATE ${_main_source_files})
target_link_libraries(contour PRIVATE contour_core)

set_target_properties(contour PROPERTIES AUTOMOC ON)
set_target_properties(contour PROPERTIES AUTORCC ON)

target_include_directories(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# {{{ declare compiler definitions
# QML debugging can be enabled at runtime via -qmljsdebugger=... or QT_QML_DEBUG env var.
# Avoid compile-time defines that force the "QML debugging is enabled" message on every
# invocation, including headless CLI commands like `contour cat`.
# These are attached to contour_core (which compiles the sources) as PUBLIC so the executable and
# the test target both inherit them when they consume the object library.
target_compile_definitions(contour_core PUBLIC
    CONTOUR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CONTOUR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CONTOUR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    CONTOUR_APP_ID="${AppId}"
)

# Disable all deprecated Qt functions prior to Qt 6.0
target_compile_definitions(contour_core PUBLIC QT_DISABLE_DEPRECATED_BEFORE=0x050F00)

# Create custom target to execute contour
add_custom_target(run COMMAND contour)

if(CONTOUR_PERF_STATS)
    target_compile_definitions(contour_core PUBLIC CONTOUR_PERF_STATS)
endif()

if(CONTOUR_WITH_TTS)
    target_compile_definitions(contour_core PUBLIC CONTOUR_WITH_TTS)
    target_link_libraries(contour_core PUBLIC Qt6::TextToSpeech)
endif()

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    target_compile_definitions(contour_core PUBLIC CONTOUR_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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
    target_link_directories(contour_core PUBLIC "/usr/local/lib")
endif()

if (TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_LIBRARIES yaml-cpp::yaml-cpp)
else()
    set(YAML_CPP_LIBRARIES yaml-cpp)
endif()

# Link dependencies go on contour_core as PUBLIC so they propagate to the `contour` executable and
# the `contour_gui_test` target through the object library's usage requirements.
target_link_libraries(contour_core
    PUBLIC
    crispy::core
    vtbackend
    vtworkspace
    Qt6::Core
    ${YAML_CPP_LIBRARIES}
    PRIVATE
    vthost
)

# {{{ GUI: Declare Qt build dependencies
if(OPENBSD)
        find_package(X11 REQUIRED)
    endif()
    target_link_libraries(contour_core
        PUBLIC
        vtrasterizer
        ContourTerminalDisplay
        Qt6::DBus
        Qt6::Multimedia
        Qt6::Network
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Widgets
    )

    if(OPENBSD)
        target_link_libraries(contour_core PUBLIC X11::xcb X11::xcb_cursor)
    elseif(NOT(WIN32) AND NOT(APPLE))
        target_link_libraries(contour_core PUBLIC xcb)
    endif()
    if(CONTOUR_BUILD_STATIC)
        qt_import_qml_plugins(contour)
    endif()

    if(LINUX AND CONTOUR_WAYLAND)
        find_package(Qt6 COMPONENTS WaylandClient WaylandClientPrivate)
        target_compile_definitions(contour_core PUBLIC CONTOUR_WAYLAND)
        target_link_libraries(contour_core PUBLIC Qt6::WaylandClient Qt6::WaylandClientPrivate)
        # The Wayland protocol client sources are generated against contour_core (which owns the
        # frontend translation units that reference them).
        qt6_generate_wayland_protocol_client_sources(contour_core FILES "${CMAKE_CURRENT_SOURCE_DIR}/protocols/blur.xml")
        # Explicitly add the generated protocol source code to the target, as the macro sometimes fails to do so.
        target_sources(contour_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/wayland-blur-protocol.c")
        target_sources(contour_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/qwayland-blur.cpp")
    endif()

    # {{{ GUI frontend E2E tests (run under the Qt offscreen platform; no display server needed)
    option(CONTOUR_GUI_TESTING "Enables building of GUI frontend unittests [default: ON]" ${CONTOUR_TESTING})
    if(CONTOUR_GUI_TESTING)
        enable_testing()
        # Qt Test provides QAbstractItemModelTester, used to machine-verify the tab list-model's
        # QAbstractListModel contract (begin/end insert/remove/move pairing) in TabListModel_test.
        find_package(Qt6 COMPONENTS Test REQUIRED)
        # The test links contour_core (the frontend object library) so it can construct real
        # TerminalSession / ContourGuiApp objects (TerminalSession_test) in addition to the Qt-free
        # surrogates. contour_core already compiles the QML resources, so the test must NOT compile
        # resources.qrc again — doing so would duplicate the resource registration symbols.
        add_executable(contour_gui_test
            test/test_main.cpp
            test/Actions_test.cpp
            test/NativeController_test.cpp
            test/RemoteController_test.cpp
            test/TmuxController_test.cpp
            test/AudioNote_test.cpp
            test/BenignQtMessages_test.cpp
            test/CaptureScreen_test.cpp
            test/CatImageArgs_test.cpp
            test/CommandCatalog_test.cpp
            test/CommandHistory_test.cpp
            test/CommandPaletteModel_test.cpp
            test/Config_test.cpp
            test/SettingsController_test.cpp
            test/SettingsPageQml_test.cpp
            test/Accessibility_qml_test.cpp
            test/ContextMenu_test.cpp
            test/SpeechSynthesizer_test.cpp
            test/TitleBarContextMenu_test.cpp
            test/FuzzyFilter_test.cpp
            test/Shortcut_test.cpp
            test/LayoutBuilder_test.cpp
            test/ContourGuiApp_test.cpp
            test/DisplayRendering_test.cpp
            test/ContentScale_test.cpp
            test/ExitCode_test.cpp
            test/CaretGeometry_test.cpp
            test/CaretReportGate_test.cpp
            test/Helper_test.cpp
            test/HyperlinkTooltip_test.cpp
            test/HorizontalWheelGesture_test.cpp
            test/ImeQueryRect_test.cpp
            test/ViewportTextIndex_test.cpp
            test/KeyboardRouting_test.cpp
            test/MainWindowQml_test.cpp
            test/FocusRouting_test.cpp
            test/MultiWindow_test.cpp
            test/NotificationRouter_test.cpp
            test/QmlDiagnosticPredicate_test.cpp
            test/RenderingBackendSelection_test.cpp
            test/ScissorRect_test.cpp
            test/RhiVertexLayout_test.cpp
            test/ScreenshotReadback_test.cpp
            test/ScreenshotRhiReadback_test.cpp
            test/TabSizeInheritance_test.cpp
            test/TerminalSessionManager_test.cpp
            test/TerminalSession_test.cpp
            test/QmlComponents_test.cpp
            test/TabColorScheme_test.cpp
            test/TabListModel_test.cpp
            test/TabLabel_test.cpp
            test/WindowGeometry_test.cpp
            test/WindowSizing_test.cpp
        )
        set_target_properties(contour_gui_test PROPERTIES AUTOMOC ON)
        target_link_libraries(contour_gui_test
            PRIVATE
            contour_core
            Qt6::Test
            Qt6::GuiPrivate # rhi/qrhi.h + qrhigles2_p.h for the end-to-end screenshot readback test
            Catch2::Catch2
        )
        target_include_directories(contour_gui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        if(NOT WIN32)
            target_compile_options(contour_gui_test PRIVATE -Wno-c2y-extensions)
        endif()
        add_test(NAME contour_gui_test COMMAND $<TARGET_FILE:contour_gui_test>)

        # {{{ Offscreen end-to-end app runs
        # Each test boots the REAL contour binary headlessly (offscreen platform + software GL) and
        # exercises paths no unit test can construct: process entry, ContourGuiApp::run, PTY spawn,
        # the full scene-graph/RHI render loop, and clean teardown. They are the coverage source for
        # the not-headless-constructible stack (see scripts/coverage.sh) and double as lifecycle
        # smoke tests in every preset. Linux-only: they assume /bin/sh and the offscreen QPA.
        if(NOT WIN32 AND NOT APPLE)
            # The custom config drives non-default Config.cpp branches and the render paths behind
            # them (background image + blur, scrollbar, indicator status line). The image path must
            # be absolute (config paths resolve ~/variables only), hence the configured template.
            set(BACKGROUND_IMAGE_PATH "${CMAKE_SOURCE_DIR}/docs/screenshots/contour-font-ligatures.png")
            configure_file(test/e2e/coverage-config.yml.in
                           ${CMAKE_CURRENT_BINARY_DIR}/e2e/coverage-config.yml @ONLY)

            add_test(NAME contour_e2e_lifecycle
                COMMAND $<TARGET_FILE:contour>
                        early-exit-threshold 0 execute sh -c "exit 0")
            add_test(NAME contour_e2e_vt_stream
                COMMAND $<TARGET_FILE:contour>
                        early-exit-threshold 0
                        execute sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/vt-stream.sh)
            # NB: still no dump-state-at-exit e2e scenario. Two real bugs on that path were fixed
            # (dump ordering: inspect() now runs BEFORE the model prune that tears the display down;
            # and the stale QOpenGLContext gate that skipped the whole dump under the RHI render loop
            # was removed), but the screenshot half rides a deferred RHI offscreen readback that
            # only completes across scene-graph frames the offscreen platform does not schedule —
            # so the capture callback (which owns at-exit termination) never fires and the run hangs.
            # Driving that readback headlessly needs a render-loop change; tracked in ROADMAP.md.
            add_test(NAME contour_e2e_custom_config
                COMMAND $<TARGET_FILE:contour>
                        config ${CMAKE_CURRENT_BINARY_DIR}/e2e/coverage-config.yml
                        early-exit-threshold 0 execute sh -c "exit 0")
            # `setsid -w`, not a bare `sh`: the sweep runs `contour capture`, which reaches its terminal
            # peer by opening /dev/tty. That is the CONTROLLING terminal -- redirecting the script's
            # stdin/stdout does not detach from it -- so run from an interactive shell it drove the
            # developer's own terminal, right down to a permission prompt that could block the run. A
            # fresh session has no controlling terminal, which is both what the test means by "no terminal
            # peer" and what CI has anyway; -w propagates the exit status (bare setsid may fork and
            # return 0, which would make the test pass vacuously).
            add_test(NAME contour_e2e_cli
                COMMAND setsid -w sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/cli-surface.sh $<TARGET_FILE:contour>)

            # LeakSanitizer is disabled for the e2e app runs (LSAN_OPTIONS=detect_leaks=0). Booting the
            # real binary probes the audio/GPU driver stack (QMediaDevices::audioOutputs() ->
            # FFmpeg/VA-API, libcuda, libdbus), which leaves one-time global-init allocations freed by the
            # drivers' own atexit hooks AFTER LSan runs. Every leaked block is driver-internal
            # (libcuda/libdbus/stripped <unknown module> FFmpeg frames) with ZERO first-party frames, and
            # the stripped frames cannot be targeted by a suppression file. This mirrors the identical,
            # documented relaxation in scripts/run-display-tests.sh. ASan proper (use-after-free/overflow)
            # stays fully enabled here; the unit-test suites keep full leak checking.
            set_tests_properties(
                contour_e2e_lifecycle contour_e2e_vt_stream
                contour_e2e_custom_config contour_e2e_cli
                PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIBGL_ALWAYS_SOFTWARE=1;LSAN_OPTIONS=detect_leaks=0"
                    TIMEOUT 120
                    RUN_SERIAL TRUE
                    LABELS e2e)
        endif()
        # }}}
    endif()
    # }}}
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

    # yaml-cpp's runtime library soname is tied to the distro release: 0.6 on 20.04, 0.7 on 22.04,
    # and 0.8 from 24.04 onward (24.04, 26.04, …). Newer releases keep 0.8, so it is the default.
    set(DEPENDS_EXTRA ", libyaml-cpp0.8")
    if("${LSB_RELEASE_NUMBER}" STREQUAL "20.04")
        set(DEPENDS_EXTRA ", libyaml-cpp0.6")
    endif()
    if("${LSB_RELEASE_NUMBER}" STREQUAL "22.04")
        set(DEPENDS_EXTRA ", libyaml-cpp0.7")
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

# {{{ Report what this build left out (see contour_note_excluded_feature above)
get_property(_contour_excluded_features GLOBAL PROPERTY CONTOUR_EXCLUDED_FEATURES)
if(_contour_excluded_features)
    # A GLOBAL property accumulates for the whole configure run, and a gate may be evaluated more
    # than once (a re-processed subdirectory, a second find_package pass) — report each feature once.
    list(REMOVE_DUPLICATES _contour_excluded_features)
    # NB: string(CONCAT), not set() with several quoted arguments — the latter builds a LIST, and
    # expanding it back into the message would separate the pieces with a semicolon.
    list(JOIN _contour_excluded_features "\n  - " _contour_excluded_list)
    string(CONCAT _contour_notice "Some features are not built into this binary:"
                                  "\n  - ${_contour_excluded_list}")
else()
    set(_contour_notice "")
endif()

if(Qt6_VERSION VERSION_LESS CONTOUR_QT_VALIDATED_VERSION)
    # Only the second branch may claim a feature was dropped. An older Qt does not by itself exclude
    # anything -- the gates above decide that, and each one records itself -- so a build that gated
    # nothing must say so, or the warning teaches the reader to distrust it.
    string(CONCAT _contour_qt_notice
           "Qt ${Qt6_VERSION} was detected, which is older than Qt ${CONTOUR_QT_VALIDATED_VERSION}"
           " — the version Contour is built and tested against.")
    if(_contour_notice)
        message(WARNING "${_contour_qt_notice}\n${_contour_notice}"
                        "\nInstall Qt ${CONTOUR_QT_VALIDATED_VERSION} or newer to include them.")
    else()
        message(WARNING "${_contour_qt_notice} Everything Contour offers is built into this binary,"
                        " but this combination is not one CI covers.")
    endif()
elseif(_contour_notice)
    message(WARNING "${_contour_notice}")
endif()
# }}}
