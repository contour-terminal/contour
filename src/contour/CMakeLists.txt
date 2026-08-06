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

# Whether this build produces a macOS application bundle, as opposed to a plain executable.
#
# A .app exists to be double-clicked, and everything that assembles one -- the
# @executable_path/../Frameworks rpath, macdeployqt, the Info.plist keys, the icon, code signing,
# notarization and the DragNDrop disk image -- serves that window. A GUI-less build produces a
# command-line binary instead: none of that applies to it, and all of it needs a Qt that renders,
# which is the whole point of not building the frontend. So it installs like any other Unix CLI
# tool. Every bundle-only block below is gated on this rather than on APPLE.
set(CONTOUR_MACOS_APP_BUNDLE OFF)
if(APPLE AND CONTOUR_FRONTEND_GUI)
    set(CONTOUR_MACOS_APP_BUNDLE ON)
endif()

NumberToHex(${PROJECT_VERSION_MAJOR} HEX_MAJOR)
NumberToHex(${PROJECT_VERSION_MINOR} HEX_MINOR)
NumberToHex(${PROJECT_VERSION_PATCH} HEX_PATCH)

# {{{ Setup QT_COMPONENTS
# QT_COMPONENTS is the list of Qt libraries Contour requires for building.
# NB: Widgets is required for SystemTrayIcon's fallback implementation
# NB: GuiPrivate provides rhi/qrhi.h (RHI resource model) used by the display lib and the screenshot
#     readback test. It must be requested here explicitly: relying on a sibling subdirectory's
#     find_package to have populated the Qt6::GuiPrivate target is order-dependent and fails on some
#     Qt packagings (e.g. Homebrew Qt on macOS), where contour_gui_test then cannot link it.
# NB: without the GUI frontend, Qt is not looked for at all. What is left of the binary is
#     `contour daemon` and the CLI verbs, and none of them touches Qt: the message handler in
#     main.cpp is compiled out, the shell-integration scripts are embedded as C++ data rather than
#     as a Qt resource, and Config.cpp reads the environment through crispy::environment. Requiring
#     Qt for that build would put a Qt that renders in the way of the very configuration whose point
#     is not to render -- and on a platform where Qt6 does not exist, in the way of building at all.
if(CONTOUR_FRONTEND_GUI)
    # Gating GuiPrivate needs Qt6_VERSION, which nothing but find_package(Qt6) sets -- hence this
    # lookup, one line ahead of the gate that reads it.
    find_package(Qt6 COMPONENTS Core REQUIRED)
    set(QT_COMPONENTS Core Gui Qml Quick QuickControls2 Network Multimedia Widgets DBus)
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10)
        list(APPEND QT_COMPONENTS GuiPrivate)
    endif()

    message(STATUS "Qt components: ${QT_COMPONENTS}")
    find_package(Qt6 COMPONENTS ${QT_COMPONENTS} REQUIRED)
endif()
# }}}

# {{{ Optional features excluded from this build
# The Qt version CI builds and validates against. Building against an older Qt is supported, but a
# feature needing a newer Qt — or an optional Qt module that is not installed — is then silently left
# out of the binary. Silently is the problem: a packager discovers it from a bug report rather than
# from their build log.
#
# So a gate does not print its own warning. It records itself here, and the single report at the
# bottom of this file names every excluded feature at once. Adding a gated feature is adding one
# contour_note_excluded_feature() call next to the gate; the reporting needs no edit.
#
# NB: that report is printed for GUI builds only, because every gate so far belongs to the frontend.
# The first gate that can fire in a headless build has to widen it -- see the guard around it.
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
#
# Guarded, because the action lives in a context menu: a headless build has nowhere to offer it from,
# so probing for the module there would ask a build that renders nothing for a Qt that speaks, and
# then report a GUI feature as excluded from a binary that never had it.
if(CONTOUR_FRONTEND_GUI)
    find_package(Qt6 COMPONENTS TextToSpeech QUIET)
    if(Qt6TextToSpeech_FOUND)
        set(CONTOUR_WITH_TTS ON)
    else()
        contour_note_excluded_feature("Read selection aloud" "Qt6::TextToSpeech not found")
    endif()
endif()
# }}}

# The imported-target name differs between a vendored yaml-cpp and a system one, so
# resolve it once here -- before add_subdirectory(config), which puts it in contour_config's
# PUBLIC interface (Config.hpp exposes yaml-cpp's headers).
if (TARGET yaml-cpp::yaml-cpp)
    set(YAML_CPP_LIBRARIES yaml-cpp::yaml-cpp)
else()
    set(YAML_CPP_LIBRARIES yaml-cpp)
endif()

# {{{ The frontend-independent layers
# Three Qt-free libraries, lowest first: the configuration model, the command vocabulary built on
# it, and the headless CLI application. Each is a target of its own rather than a pile of sources
# in contour_core, which is what makes the layering checkable instead of merely documented --
# contour_config cannot reach a QObject, because it does not link Qt.
#
# Added before display/ and before contour_core, both of which consume them.
add_subdirectory(config)
add_subdirectory(command)
add_subdirectory(cli)

# Declared once, on the lowest layer, and inherited by every consumer through contour_config's
# PUBLIC usage requirements -- contour_command, contour_cli, contour_core, and through those the
# executable and both test binaries.
target_compile_definitions(contour_config PUBLIC
    CONTOUR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CONTOUR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CONTOUR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    CONTOUR_APP_ID="${AppId}"
)
if(CONTOUR_FRONTEND_GUI)
    target_compile_definitions(contour_config PUBLIC CONTOUR_FRONTEND_GUI)
endif()
# }}}

# input/ and platform/ are GUI-only: one translates Qt events, the other adapts to Qt and the OS.
# Both sit below display/ and session/, which is why they are added before them.
if(CONTOUR_FRONTEND_GUI)
    add_subdirectory(input)
    add_subdirectory(platform)
    add_subdirectory(display)
endif()

if(CONTOUR_MACOS_APP_BUNDLE)
    #set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
    # vcpkg gives its dylibs an `@rpath/libfoo.dylib` install name, and macdeployqt expands
    # @rpath using only the binary's own LC_RPATH -- its -libpath option is not consulted
    # for this. Without a search path that resolves, macdeployqt reports "Cannot resolve
    # rpath" and leaves openssl, freetype, harfbuzz, fontconfig and cairo out of the bundle.
    # So the deployment step gets a resolvable rpath, and the install rules delete it again
    # once macdeployqt has rewritten every reference to @executable_path/../Frameworks --
    # a build-machine path has no business shipping inside a release binary.
    if(DEFINED VCPKG_TOOLCHAIN AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(CONTOUR_MACOS_DEPLOY_RPATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
        list(APPEND CMAKE_INSTALL_RPATH "${CONTOUR_MACOS_DEPLOY_RPATH}")
    endif()
    set(CMAKE_BUILD_RPATH "${CMAKE_INSTALL_RPATH}")
    set(CMAKE_MACOSX_RPATH ON)
    #set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()

set(_header_files
    session/HyperlinkTooltip.hpp
    window/TabLabel.hpp
)

# Sources are split into a reusable object library (contour_core) and the thin executable entry
# point (main.cpp). The object library carries every translation unit of the frontend that is not
# yet a module of its own, so that both the `contour` executable AND the `contour_gui_test` target
# can link the exact same objects — this is what lets the GUI tests construct real
# TerminalSession/ContourGuiApp objects instead of only Qt-free surrogates. An OBJECT library (not
# STATIC) is used deliberately: it shares the compiled objects without a static archive's
# symbol-stripping, which would otherwise drop AUTOMOC/AUTORCC registration objects whose symbols
# are only referenced reflectively at runtime.
set(_core_source_files
    session/HyperlinkTooltip.cpp
    window/TabLabel.cpp
)
# The executable entry point lives outside the object library so the test can supply its own main
# (Catch2's) without a duplicate-symbol clash.
set(_main_source_files
    main.cpp
)

# Everything below opens a window, drives one, or talks to a daemon on behalf of one. Without
# CONTOUR_FRONTEND_GUI the `contour` binary keeps its CLI verbs -- `contour daemon` among them,
# which is why a GUI-less build is worth having at all.
if(CONTOUR_FRONTEND_GUI)
    list(APPEND _header_files
        window/CommandPaletteModel.hpp
        window/ContextMenuModel.hpp
        ContourGuiApp.hpp
        session/ExitCode.hpp
        config/LayoutBuilder.hpp
        config/LayoutStore.hpp
        session/PaneProxy.hpp
        session/SessionFactory.hpp
        window/SettingsController.hpp
        session/TerminalSession.hpp
        session/TerminalSessionManager.hpp
        window/UiStyleProvider.hpp
        session/SessionInput.hpp
        session/FontControl.hpp
        session/SpawnCommand.hpp
        session/Logging.hpp
        Logging.hpp
        display/RenderState.hpp
        display/RenderingBackendSelection.hpp
        geometry/CellRectangle.hpp
        geometry/WindowGeometry.hpp
        remote/NativeController.hpp
        remote/RemoteController.hpp
        remote/ReactorThread.hpp
        remote/RemoteLayout.hpp
        remote/RoutingSessionFactory.hpp
        remote/TmuxController.hpp
    )
    list(APPEND _core_source_files
        window/CommandPaletteModel.cpp
        window/ContextMenuModel.cpp
        ContourGuiApp.cpp
        session/PaneProxy.cpp
        config/LayoutBuilder.cpp
        config/LayoutStore.cpp
        session/SessionFactory.cpp
        window/SettingsController.cpp
        session/TerminalSession.cpp
        display/TerminalAccessible.cpp
        session/TerminalSessionManager.cpp
        window/UiStyleProvider.cpp
        window/WindowController.cpp
        session/SessionInput.cpp
        session/FontControl.cpp
        session/SpawnCommand.cpp
        remote/NativeController.cpp
        remote/RemoteController.cpp
        remote/RemoteLayout.cpp
        remote/TmuxController.cpp
    )
endif()

if(WIN32)
    list(APPEND _main_source_files packaging/contour.rc)
endif()

# Both QML modules exist only for the GUI frontend, so they are built only for it. Outside that
# guard a headless CLI-only build still ran qmlcachegen over all 45 QML files and compiled the
# ~3.6 MB of C++ it generates -- about 110 seconds of a clean build for code such a build can
# never load.
if(CONTOUR_FRONTEND_GUI)
    # {{{ Contour.Ui: the application's own QML, as a proper QML module
    # A module rather than a pile of files in resources.qrc: it gives the components a URI to be imported
    # by (so one is referenced by type name rather than by qrc path, and QML tooling can resolve them),
    # it lets qmlcachegen compile them ahead of time, and a module is the shape a custom Qt Quick
    # Controls style has to take.
    #
    # Its own target rather than contour_core, because qt_add_qml_module needs a real library to back the
    # module and contour_core is an OBJECT library. STATIC so it still links into both consumers (the
    # contour executable and contour_gui_test) without a second copy of the QML.
    #
    # NB: a QML module's type name is its file's stem, which must therefore start uppercase -- that is why
    # the window is Main.qml rather than the historical main.qml.
    set(_qml_files
        qml/Main.qml
        qml/BellSound.qml
        qml/RequestPermission.qml
        qml/SessionChrome.qml
        qml/TitleBar.qml
        qml/TabStrip.qml
        qml/NewTabMenu.qml
        qml/TabItem.qml
        qml/TabContextMenu.qml
        qml/TabColorFlyout.qml
        qml/WindowControls.qml
        qml/ResizeBorder.qml
        qml/PaneNode.qml
        qml/TerminalPane.qml
        qml/ActionContextMenu.qml
        qml/CommandPalette.qml
        qml/SaveLayoutDialog.qml
        qml/SettingsPage.qml
        qml/SettingsNavItem.qml
        qml/SettingsListItem.qml
        qml/ConfirmDialog.qml
        qml/SettingRow.qml
        qml/SettingsSection.qml
        qml/ColorSchemeEditor.qml
        qml/StatusLineIndicatorEditor.qml
        qml/StatusLineSegmentRow.qml
        qml/StatusLineChip.qml
        qml/StatusLineItemDialog.qml
        qml/StatusLinePreviewItem.qml
        qml/ColorSlotField.qml
    )

    # The files live in ui/ but the module is flat: without an alias each one would land at
    # :/qt/qml/Contour/Ui/qml/<Name>.qml, leaving a stray directory level in every resource path and in
    # the qmldir. Aliasing to the bare file name makes the resource layout match the module identity.
    foreach(_qml_file IN LISTS _qml_files)
        get_filename_component(_qml_name "${_qml_file}" NAME)
        set_source_files_properties("${_qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${_qml_name}")
    endforeach()

    # This module is QML files and nothing else, which is what both NO_ options say. The C++ types the
    # QML talks to (ContourTerminal, TerminalSession, WindowController, ...) are registered by hand into
    # the separate Contour.Terminal URI from ContourGuiApp, because their compilation unit is contour_core
    # -- an OBJECT library, which qt_add_qml_module cannot back.
    #
    # NO_GENERATE_QMLTYPES: without it Qt asks this target for metatypes it has no C++ sources to produce,
    # and configuration fails demanding AUTOMOC.
    #
    # NO_PLUGIN: a plugin exists to register a module's C++ types, and there are none here. Left in, the
    # generated qmldir declares `optional plugin contour_qmlplugin`, which the engine then fails to load
    # because a STATIC module's plugin has to be linked and registered into every consuming executable --
    # and every QML component fails with `module "Contour.Ui" plugin ... not found`. Without it the module
    # resolves from its own qmldir under the default :/qt/qml import path, which is all it needs.
    # OUTPUT_DIRECTORY mirrors the URI: two QML modules in one CMake directory would otherwise both want
    # to drop their qmldir straight into the binary dir, which CMake rejects outright.
    qt_add_qml_module(contour_qml
        STATIC
        URI Contour.Ui
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Contour/Ui
        NO_GENERATE_QMLTYPES
        NO_PLUGIN
        QML_FILES ${_qml_files}
    )
    # }}}

    # {{{ ContourTui: the Qt Quick Controls style that paints controls as terminal chrome
    # A Qt Quick Controls style IS a QML module whose URI is the style name, so that QQuickStyle can find
    # it on the import path -- which is the second reason the QML moved to qt_add_qml_module (see above).
    # Selecting it is app-wide and once-per-process (ContourGuiApp), which is why `ui_style` takes effect
    # on the next start.
    #
    # The same NO_GENERATE_QMLTYPES / NO_PLUGIN reasoning as Contour.Ui applies: QML files, no C++ types.
    set(_tui_style_files
        styles/ContourTui/Button.qml
        styles/ContourTui/CheckBox.qml
        styles/ContourTui/ComboBox.qml
        styles/ContourTui/ItemDelegate.qml
        styles/ContourTui/Label.qml
        styles/ContourTui/Menu.qml
        styles/ContourTui/MenuItem.qml
        styles/ContourTui/MenuSeparator.qml
        styles/ContourTui/Popup.qml
        styles/ContourTui/ScrollBar.qml
        styles/ContourTui/ScrollView.qml
        styles/ContourTui/Switch.qml
        styles/ContourTui/TextField.qml
        styles/ContourTui/ToolButton.qml
        styles/ContourTui/ToolTip.qml
    )

    foreach(_style_file IN LISTS _tui_style_files)
        get_filename_component(_style_name "${_style_file}" NAME)
        set_source_files_properties("${_style_file}" PROPERTIES QT_RESOURCE_ALIAS "${_style_name}")
    endforeach()

    qt_add_qml_module(contour_tui_style
        STATIC
        URI ContourTui
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml
        OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ContourTui
        NO_GENERATE_QMLTYPES
        NO_PLUGIN
        QML_FILES ${_tui_style_files}
    )
    # }}}

endif()

# Everything in resources.qrc is GUI-only (the bell sounds and the window icon), so a GUI-less build
# neither compiles it nor needs the resource compiler that would come with the Qt package.
set(_qt_resources "")
if(CONTOUR_FRONTEND_GUI)
    set(_qt_resources resources.qrc)
    qt_add_resources(_qt_resources ${_qt_resources})
endif()

source_group(Sources FILES ${_core_source_files} ${_main_source_files})
source_group(Headers FILES ${_header_files})
source_group(Resources FILES ${_qt_resources})
source_group(QML FILES ${_qml_files})

# {{{ contour_core: the reusable object library (everything but main.cpp)
add_library(contour_core OBJECT)
target_sources(contour_core PRIVATE ${_core_source_files} ${_header_files} ${_qt_resources})

# Both the binary dir (for generated headers) and the parent src dir (so "#include <contour/...>"
# resolves) must be PUBLIC so consumers of the object library inherit them.
target_include_directories(contour_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
# }}}

# qt_add_executable() is a Qt command, and it does Qt things -- QML plugin importing, the Android and
# macOS finalizers. A GUI-less build wants a plain command-line executable and, more to the point,
# must not need the Qt CMake package to declare one.
if(CONTOUR_FRONTEND_GUI)
    qt_add_executable(contour)
else()
    add_executable(contour)
endif()
target_sources(contour PRIVATE ${_main_source_files})
target_link_libraries(contour PRIVATE contour_core)

# Everything Qt about these two targets, in one place. moc and rcc are Qt tools and without the GUI
# frontend there is nothing for either to do: every Q_OBJECT in this directory belongs to a GUI-only
# translation unit, and resources.qrc is not compiled above. Leaving them on would merely ask a
# Qt-free build for Qt's code generators.
if(CONTOUR_FRONTEND_GUI)
    set_target_properties(contour_core contour PROPERTIES AUTOMOC ON AUTORCC ON)
    # Disable all deprecated Qt functions prior to Qt 6.0.
    target_compile_definitions(contour_core PUBLIC QT_DISABLE_DEPRECATED_BEFORE=0x050F00)
endif()

target_include_directories(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# {{{ declare compiler definitions
# QML debugging can be enabled at runtime via -qmljsdebugger=... or QT_QML_DEBUG env var.
# Avoid compile-time defines that force the "QML debugging is enabled" message on every
# invocation, including headless CLI commands like `contour cat`.
#
# The version/app-identity definitions are NOT repeated here: they are declared once on
# contour_config (see the top of this file) and reach contour_core, the executable and both test
# binaries through its PUBLIC usage requirements. Only what is genuinely frontend-specific lives
# below.

# Create custom target to execute contour
add_custom_target(run COMMAND contour)

if(CONTOUR_PERF_STATS)
    target_compile_definitions(contour_core PUBLIC CONTOUR_PERF_STATS)
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
elseif(CONTOUR_MACOS_APP_BUNDLE)
    # qt_add_executable() sets MACOSX_BUNDLE_INFO_PLIST, so Info.plist comes from Qt's
    # template and only the properties it substitutes have any effect -- which excludes
    # the deprecated CFBundleGetInfoString and CFBundleLongVersionString. It supplies
    # LSMinimumSystemVersion from CMAKE_OSX_DEPLOYMENT_TARGET, and omits
    # NSHighResolutionCapable, whose absence means enabled. See
    # docs/internals/macos-code-signing.md.
    set_target_properties(contour PROPERTIES
        OUTPUT_NAME "contour"
        MACOSX_RPATH ON
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Contour"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${AppId}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${CONTOUR_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${CONTOUR_VERSION}"
        XCODE_ATTRIBUTE_PRODUCT_NAME "Contour Terminal Emulator"
        # Names the icon the install rules already place at Contents/Resources/contour.icns.
        # Without it CFBundleIconFile renders empty and both the app and the disk image
        # show the generic executable icon.
        MACOSX_BUNDLE_ICON_FILE "contour.icns"
    )
endif()
# }}}

if(FREEBSD)
    # FreeBSD does not find yaml-cpp in /usr/local but weirdly everything else.
    target_link_directories(contour_core PUBLIC "/usr/local/lib")
endif()

# Link dependencies go on contour_core as PUBLIC so they propagate to the `contour` executable and
# the `contour_gui_test` target through the object library's usage requirements.
#
# contour_command and contour_cli carry the layers beneath the frontend, and contour_config comes
# with them -- along with crispy, vtbackend, vtpty, vtrasterizer, vtworkspace and yaml-cpp, which
# are its PUBLIC interface and so need no restating here. vtworkspace is named anyway because the
# frontend's own sources (the session manager's tab/pane tree) use it directly rather than through
# the configuration model.
target_link_libraries(contour_core
    PUBLIC
    contour_command
    contour_cli
    vtworkspace
    PRIVATE
    vthost
)

# The GUI-only layers beneath the frontend. Guarded for the same reason they are only added as
# subdirectories under it: without a frontend there is no Qt event to translate and no compositor,
# audio device or notification bus to adapt to.
if(CONTOUR_FRONTEND_GUI)
    target_link_libraries(contour_core PUBLIC contour_input contour_platform)
endif()

# {{{ GUI: Declare Qt build dependencies
# The QML modules are GUI-only (see the guard above), so the dependency on them is too -- as is
# Qt6::Core itself, which no longer has a single caller outside the frontend.
if(CONTOUR_FRONTEND_GUI)
    if(OPENBSD)
        find_package(X11 REQUIRED)
    endif()
    target_link_libraries(contour_core
        PUBLIC
        contour_qml
        contour_tui_style
        ContourTerminalDisplay
        Qt6::Core
        Qt6::DBus
        Qt6::Multimedia
        Qt6::Network
        Qt6::Qml
        Qt6::QuickControls2
        Qt6::Widgets
        Qt6::QuickTemplates2
    )

    # xcb, Carbon and the Wayland blur protocol are declared by the targets that actually use them:
    # xcb and Wayland by contour_platform (BlurBehind.cpp is the only caller of either), Carbon by
    # contour_input (KeyboardLayout.cpp is the only caller). Restating them here would have generated
    # the Wayland protocol sources a second time, into a target that no longer holds the .xml.
    if(CONTOUR_BUILD_STATIC)
        qt_import_qml_plugins(contour)
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
            remote/NativeController_test.cpp
            remote/RemoteController_test.cpp
            remote/TmuxController_test.cpp
            platform/AudioNote_test.cpp
            test/BenignQtMessages_test.cpp
            window/CommandPaletteModel_test.cpp
            config/Config_test.cpp
            window/SettingsController_test.cpp
            test/SettingsPageQml_test.cpp
            test/Accessibility_qml_test.cpp
            platform/SpeechSynthesizer_test.cpp
            test/ContourGuiApp_test.cpp
            display/DisplayRendering_test.cpp
            display/ContentScale_test.cpp
            session/ExitCode_test.cpp
            display/CaretReportGate_test.cpp
            display/TerminalAccessible_test.cpp
            session/SessionInput_test.cpp
            session/HyperlinkTooltip_test.cpp
            input/HorizontalWheelGesture_test.cpp
            display/ImeQueryRect_test.cpp
            display/ViewportTextIndex_test.cpp
            session/KeyboardRouting_test.cpp
            test/MainWindowQml_test.cpp
            session/FocusRouting_test.cpp
            window/MultiWindow_test.cpp
            platform/NotificationRouter_test.cpp
            platform/TaskbarProgressRouter_test.cpp
            test/QmlDiagnosticPredicate_test.cpp
            display/RenderingBackendSelection_test.cpp
            display/ScissorRect_test.cpp
            display/RhiVertexLayout_test.cpp
            display/ScreenshotReadback_test.cpp
            display/ScreenshotRhiReadback_test.cpp
            session/TabSizeInheritance_test.cpp
            session/TerminalSessionManager_test.cpp
            session/TerminalSession_test.cpp
            test/QmlComponents_test.cpp
            window/TabColorScheme_test.cpp
            test/ContourTuiStyle_test.cpp
            window/UiStyleProvider_test.cpp
            window/TabListModel_test.cpp
            window/TabLabel_test.cpp
            geometry/CellRectangle_test.cpp
            geometry/WindowGeometry_test.cpp
            window/WindowSizing_test.cpp
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
            # The only test that starts a real daemon. vthost_test drives runDaemon's internals
            # through injected sockets, which cannot reach the wiring between the CLI verb and the
            # daemon: argument validation, socket path derivation, SIGTERM handling, unlink-on-close.
            # `setsid -w` for the same reason as the sweep above -- a daemon must not inherit the
            # developer's controlling terminal.
            add_test(NAME contour_e2e_daemon
                COMMAND setsid -w sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/daemon-lifecycle.sh $<TARGET_FILE:contour>)

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
                contour_e2e_custom_config contour_e2e_cli contour_e2e_daemon
                PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIBGL_ALWAYS_SOFTWARE=1;LSAN_OPTIONS=detect_leaks=0"
                    TIMEOUT 120
                    RUN_SERIAL TRUE
                    LABELS e2e)
            # Also selectable with the daemon suites: `ctest -L daemon` must cover the feature
            # end to end, not just its unit layers.
            set_property(TEST contour_e2e_daemon APPEND PROPERTY LABELS daemon)
        endif()
        # }}}
    endif()
    # }}}
endif()
# }}}

# {{{ contour_test: the Qt-free unit tests, in BOTH frontend configurations
# config/, command/ and cli/ link no Qt, so their tests need none either -- and a build configured
# without the GUI frontend has none to give them. That is how that configuration came to have no
# unit tests at all: every one of them lived in contour_gui_test, which needs a QGuiApplication.
#
# Config_test stays with the GUI tests deliberately: besides the configuration model it also asserts
# the Qt colour-scheme mapping and uses qputenv, so it is not Qt-free however Qt-free the model is.
if(CONTOUR_TESTING)
    enable_testing()
    add_executable(contour_test
        test/core_test_main.cpp
        config/Actions_test.cpp
        config/LayoutBuilder_test.cpp
        config/UiStyle_test.cpp
        command/CommandCatalog_test.cpp
        command/CommandHistory_test.cpp
        command/ContextMenu_test.cpp
        command/FuzzyFilter_test.cpp
        command/Shortcut_test.cpp
        command/TitleBarContextMenu_test.cpp
        cli/CaptureScreen_test.cpp
        cli/CatImageArgs_test.cpp
        cli/ShellIntegration_test.cpp
    )
    target_link_libraries(contour_test PRIVATE contour_command contour_cli Catch2::Catch2)
    target_include_directories(contour_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(contour_test PRIVATE -Wno-c2y-extensions)
    endif()
    add_test(NAME contour_test COMMAND $<TARGET_FILE:contour_test>)
endif()
# }}}

# {{{ e2e: the CLI verb surface, in BOTH frontend configurations
# Deliberately outside the CONTOUR_FRONTEND_GUI block above: what this asserts is which verbs each
# configuration exposes, so it has to run in the configuration that has no GUI too -- that is the
# one where a missing guard shows up.
#
# A CMake script rather than a shell script (as the other e2e tests are), because it must also run
# on Windows, where `sh` is not available and where the daemon gets the least coverage otherwise.
if(CONTOUR_TESTING)
    # Registers one `cmake -P` end-to-end test against the built contour binary.
    #
    # The verdict is MARKER, not the exit code: `cmake -P` returns 0 even after message(FATAL_ERROR)
    # before CMake 3.31, so a failing assertion would otherwise pass silently. CONTOUR_BINARY is
    # passed for every such test, and QT_QPA_PLATFORM=offscreen set for every one: in a GUI build
    # these run the Qt-linked binary, and a CI runner has no display for it to fall back on.
    #
    # NAME   Test name, also naming test/e2e/<NAME>.cmake's sibling script via SCRIPT.
    # SCRIPT The script under test/e2e/ to run.
    # MARKER The success line the script prints last.
    # ARGS   Further `-D VAR=value` arguments, script-specific.
    function(contour_add_e2e_cmake_test)
        cmake_parse_arguments(E2E "" "NAME;SCRIPT;MARKER" "ARGS" ${ARGN})
        add_test(NAME ${E2E_NAME}
            COMMAND ${CMAKE_COMMAND}
                    -D "CONTOUR_BINARY=$<TARGET_FILE:contour>"
                    ${E2E_ARGS}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/${E2E_SCRIPT})
        set_tests_properties(${E2E_NAME} PROPERTIES
            TIMEOUT 60
            LABELS e2e
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            PASS_REGULAR_EXPRESSION "${E2E_MARKER}")
    endfunction()

    if(CONTOUR_FRONTEND_GUI)
        set(_frontend_kind "gui")
    else()
        set(_frontend_kind "headless")
    endif()
    contour_add_e2e_cmake_test(
        NAME contour_e2e_cli_verbs
        SCRIPT CliVerbs.cmake
        MARKER "cli-verbs: OK"
        ARGS -D "FRONTEND=${_frontend_kind}")

    # Registered outside the guard for the same reason: `generate integration` is a verb the GUI-less
    # build keeps, and its scripts are embedded by EmbedShellIntegration.cmake for both configurations.
    contour_add_e2e_cmake_test(
        NAME contour_e2e_shell_integration
        SCRIPT ShellIntegration.cmake
        MARKER "shell-integration: OK"
        ARGS -D "SHELLS=${CONTOUR_SHELL_INTEGRATION_SHELLS}"
             -D "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cli"
             -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}")
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

# {{{ macOS signing and notarization settings
# Declared here rather than next to their use in the install() rules below, because the
# CPack block needs them too and CMake reads this file top to bottom.
if(APPLE)
    # "-" is an ad-hoc signature: enough to run locally, not enough to distribute.
    # A release build passes the Developer ID; see cmake/presets/os-macos.json.
    #
    # Declared for every macOS build rather than only for bundle ones, because every macOS preset
    # sets it: a cache entry no target reads is merely unused, but one the project never declares is
    # reported back to the user as a variable they got wrong.
    set(CODE_SIGN_CERTIFICATE_ID "-" CACHE STRING "macOS code signing identity, or '-' for ad-hoc")
endif()

if(CONTOUR_MACOS_APP_BUNDLE)
    option(CONTOUR_MACOS_NOTARIZE "Submit the macOS disk image to Apple's notary service" OFF)
    # A second Apple round trip, so that the app dragged out of the image carries its own
    # ticket and launches offline. Worth its minute on something users download; not worth
    # paying on every push. @see cmake/MacOSNotarizeApp.cmake
    option(CONTOUR_MACOS_STAPLE_APP "Also notarize and staple the app inside the image" OFF)
    set(CONTOUR_MACOS_NOTARY_PROFILE "contour-notary" CACHE STRING
        "notarytool keychain profile (see `xcrun notarytool store-credentials`)")

    # The oldest macOS the produced .dmg promises to run on, checked against what the
    # bundled binaries actually demand. Empty means "report the floor, do not enforce it",
    # which is right for a dev machine -- the floor there is whatever the developer's
    # Homebrew bottles were built for and is nobody's promise. A release build sets it, so
    # that an artifact users cannot launch fails the build instead of reaching them.
    set(CONTOUR_MACOS_MIN_SUPPORTED "" CACHE STRING
        "Oldest macOS the package must run on, e.g. 15.0 (empty: report only)")

    # Notarizing an ad-hoc-signed app is rejected by Apple, but only after a multi-minute
    # wait -- so reject the combination here instead.
    if((CONTOUR_MACOS_NOTARIZE OR CONTOUR_MACOS_STAPLE_APP)
       AND CODE_SIGN_CERTIFICATE_ID STREQUAL "-")
        message(FATAL_ERROR
            "CONTOUR_MACOS_NOTARIZE requires a real CODE_SIGN_CERTIFICATE_ID; "
            "Apple rejects ad-hoc signatures. Set a Developer ID identity, or turn "
            "notarization off.")
    endif()

    set(CONTOUR_MACOS_BUNDLE_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/macos-bundle.py")

    # REQUIRED, unlike the QUIET lookup at the top level: scripts/macos-bundle.py is what
    # verifies, signs and notarizes the bundle, so without an interpreter there is no way
    # to produce a distributable artifact at all. Failing at configure time beats failing
    # halfway through an install.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
# }}}

# {{{ CPACK variable definitions
if(NOT(CPACK_GENERATOR))
    # A disk image is a way to hand someone an app to drag into /Applications; without a bundle to
    # put in it there is nothing to drag, so a GUI-less macOS build falls through to CPack's own
    # default (a tarball) like any other Unix CLI tool.
    if(CONTOUR_MACOS_APP_BUNDLE)
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
    set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/wix_patch.xml")
    set(CPACK_WIX_PROPERTY_WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT "Launch Contour")
endif()
if(CONTOUR_MACOS_APP_BUNDLE)
    set(CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/images/contour-logo.icns")
    set(CPACK_DMG_VOLUME_NAME "Contour")

    # The disk image needs its own signature and its own notarization ticket: CPack
    # assembles it *after* install(), so the signature the install step applies to
    # contour.app does not cover the .dmg wrapping it. Both hooks read the settings
    # below out of the CPack config, hence the CPACK_-prefixed names.
    set(CPACK_CONTOUR_CODE_SIGN_IDENTITY "${CODE_SIGN_CERTIFICATE_ID}")
    set(CPACK_CONTOUR_NOTARIZE "${CONTOUR_MACOS_NOTARIZE}")
    set(CPACK_CONTOUR_STAPLE_APP "${CONTOUR_MACOS_STAPLE_APP}")
    set(CPACK_CONTOUR_NOTARY_PROFILE "${CONTOUR_MACOS_NOTARY_PROFILE}")
    set(CPACK_CONTOUR_BUNDLE_SCRIPT "${CONTOUR_MACOS_BUNDLE_SCRIPT}")
    set(CPACK_CONTOUR_PYTHON "${Python3_EXECUTABLE}")
    # Runs on the staged bundle before hdiutil, so the ticket ends up inside the image.
    set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/MacOSNotarizeApp.cmake")
    # Runs on the finished .dmg.
    set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/MacOSSignDmg.cmake")
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
# }}}

if(WIN32)
    install(TARGETS contour DESTINATION bin)
    # windeployqt stages the Qt runtime beside the executable, and the directory install below exists
    # to pick that staging up. Both are gated, because a GUI-less build links no Qt: there would be
    # nothing to deploy, DeployQt.cmake's find_package(Qt6Core REQUIRED) would put Qt back in the way
    # of the one configuration whose point is not to need it, and what remains to install is the
    # single command-line executable already installed above. This is the Windows twin of the
    # app-bundle gate that CONTOUR_MACOS_APP_BUNDLE makes below.
    if(CONTOUR_FRONTEND_GUI)
        include(DeployQt)
        windeployqt(contour)
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
    endif()
elseif(CONTOUR_MACOS_APP_BUNDLE)
    # See: https://stackoverflow.com/questions/35612687/cmake-macos-x-bundle-with-bundleutiliies-for-qt-application/48035834#48035834
    set(APP_NAME contour)
    set(App_Contents "${APP_NAME}.app/Contents")
    set(INSTALL_RUNTIME_DIR "${App_Contents}/MacOS")
    set(INSTALL_CMAKE_DIR "${App_Contents}/Resources")

    # Install application icon
    install(FILES "res/images/contour-logo.icns" DESTINATION "${INSTALL_CMAKE_DIR}" RENAME "contour.icns")
    install(DIRECTORY "${terminfo_basedir}" DESTINATION "${INSTALL_CMAKE_DIR}")
    install(DIRECTORY "cli/shell-integration" DESTINATION "${INSTALL_CMAKE_DIR}")

    # add_custom_target(Docs SOURCES README.md LICENSE.txt)
    # TODO: install(TARGETS Docs ...)

    # Destination paths below are relative to ${CMAKE_INSTALL_PREFIX}
    install(TARGETS ${APP_NAME}
        BUNDLE DESTINATION . COMPONENT Runtime
        RUNTIME DESTINATION "${INSTALL_RUNTIME_DIR}" COMPONENT Runtime
    )

    include(DeployQt)  # Just to get access to ${MACDEPLOYQT_EXECUTABLE}
    get_filename_component(_macdeployqt_path "${MACDEPLOYQT_EXECUTABLE}" PATH)
    message(STATUS "macdeployqt location: ${MACDEPLOYQT_EXECUTABLE}")

    # Applied to contour.app only, never to nested code -- entitlements belong to the
    # outermost signature. Both keys it contains relax hardened-runtime restrictions:
    # allow-jit for the QML engine's V4 JIT (MAP_JIT), and disable-library-validation as
    # insurance for Qt plugin loading. The file itself carries no comments explaining
    # this, because the kernel's entitlements parser (AMFI) rejects XML comments outright;
    # docs/internals/macos-code-signing.md has the reasoning.
    set(_entitlements "${CMAKE_SOURCE_DIR}/support/macOS/entitlements.plist")

    # Deploy, drop the plugins that cannot load, prove the result is self-contained,
    # then sign it -- in that order, and with every step fatal on failure. macdeployqt
    # reports unresolvable frameworks on stderr but still exits 0, so without the verify
    # step a bundle missing half its Qt frameworks used to sail through the build and
    # fail on the user's machine.
    install(CODE "
        set(_app \"\${CMAKE_INSTALL_PREFIX}/contour.app\")

        execute_process(
            WORKING_DIRECTORY \"${_macdeployqt_path}/..\" # macdeployqt resolves its QML imports relative to this
            COMMAND ${MACDEPLOYQT_EXECUTABLE} \"\${_app}\"
                    -always-overwrite
                    -verbose=1
                    -no-strip
                    \"-qmldir=${CMAKE_CURRENT_SOURCE_DIR}/qml\"
            COMMAND_ERROR_IS_FATAL ANY
        )

        # The deployment-only rpath from the top of this file has done its job; every
        # reference now points into the bundle. Leaving it would ship an absolute path to
        # the build machine's vcpkg tree inside the released binary. macdeployqt usually
        # strips the rpaths it consumed itself, so this only has to handle the case where
        # it did not -- hence the check rather than an unconditional delete, which fails
        # hard on a missing LC_RPATH.
        if(NOT \"${CONTOUR_MACOS_DEPLOY_RPATH}\" STREQUAL \"\")
            execute_process(
                COMMAND otool -l \"\${_app}/Contents/MacOS/contour\"
                OUTPUT_VARIABLE _load_commands
                COMMAND_ERROR_IS_FATAL ANY
            )
            string(FIND \"\${_load_commands}\" \"${CONTOUR_MACOS_DEPLOY_RPATH}\" _deploy_rpath_at)
            if(NOT _deploy_rpath_at EQUAL -1)
                message(STATUS \"Removing deployment-only rpath from contour\")
                execute_process(
                    COMMAND install_name_tool -delete_rpath \"${CONTOUR_MACOS_DEPLOY_RPATH}\"
                            \"\${_app}/Contents/MacOS/contour\"
                    COMMAND_ERROR_IS_FATAL ANY
                )
            endif()
        endif()

        execute_process(
            COMMAND \"${Python3_EXECUTABLE}\" \"${CONTOUR_MACOS_BUNDLE_SCRIPT}\" prune \"\${_app}\"
            COMMAND_ERROR_IS_FATAL ANY
        )

        set(_verify_args)  # unset, not \"\": an empty string is a list element, and would
                           # reach the script as an empty argv entry
        if(NOT \"${CONTOUR_MACOS_MIN_SUPPORTED}\" STREQUAL \"\")
            list(APPEND _verify_args --max-minimum-os \"${CONTOUR_MACOS_MIN_SUPPORTED}\")
        endif()
        execute_process(
            COMMAND \"${Python3_EXECUTABLE}\" \"${CONTOUR_MACOS_BUNDLE_SCRIPT}\" verify \"\${_app}\"
                    \${_verify_args}
            COMMAND_ERROR_IS_FATAL ANY
        )

        execute_process(
            COMMAND \"${Python3_EXECUTABLE}\" \"${CONTOUR_MACOS_BUNDLE_SCRIPT}\" sign \"\${_app}\"
                    --identity \"${CODE_SIGN_CERTIFICATE_ID}\"
                    --entitlements \"${_entitlements}\"
            COMMAND_ERROR_IS_FATAL ANY
        )
    ")
else()
    # Any other Unix -- and, since the branch above now takes only bundle builds, a GUI-less macOS
    # build too. That one produces a plain command-line binary, so it installs like any other Unix
    # CLI tool: the executable and the data every platform wants, first.
    include(GNUInstallDirs)
    install(TARGETS contour DESTINATION bin)
    install(DIRECTORY "cli/shell-integration" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(FILES "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(DIRECTORY "${terminfo_basedir}" DESTINATION "${CMAKE_INSTALL_DATADIR}")

    # What a macOS build must NOT pick up is everything addressed at a freedesktop desktop
    # environment (the .desktop entries, the KIO service menus, the hicolor icons, the AppStream
    # metainfo) or at Debian packaging: macOS reads none of it, and `lsb_release` is not there to
    # ask. Hence this one guard rather than a second, near-identical branch.
    if(NOT APPLE)
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

        install(FILES "packaging/contour.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" RENAME "${AppId}.desktop")
        install(FILES "packaging/contour-run.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/kio/servicemenus" RENAME "${AppId}.RunIn.desktop")
        install(FILES "packaging/${AppId}.OpenHere.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/kio/servicemenus")
        install(FILES "res/images/contour-logo-512.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps" RENAME "${AppId}.png")
        install(FILES "res/images/contour-logo-256.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps" RENAME "${AppId}.png")
        install(FILES "res/images/contour-logo-128.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/128x128/apps" RENAME "${AppId}.png")
        install(FILES "res/images/contour-logo-64.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps" RENAME "${AppId}.png")
        install(FILES "res/images/contour-logo-32.png" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/32x32/apps" RENAME "${AppId}.png")
        install(FILES "${PROJECT_SOURCE_DIR}/metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo" RENAME "${AppId}.metainfo.xml")
    endif()
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
# GUI-only, because both halves of this report are claims about the GUI stack: every gated feature
# is one, and CONTOUR_QT_VALIDATED_VERSION names the Qt the frontend is tested against. The headless
# job builds against whatever Qt6::Core the distro ships -- deliberately, to prove that build needs
# nothing more -- so telling it that its Qt is older than the one CI covers would be simply untrue.
if(CONTOUR_FRONTEND_GUI)
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
endif()
# }}}
