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

option(CONTOUR_MACOS_DEPLOY "Run macdeployqt + prune/verify/sign on the macOS app bundle" ON)

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
    # Qt warns, once per private module and on every configure, that a project using private headers
    # is tied to the exact Qt build it compiled against. That is true and it is deliberate: rhi/qrhi.h
    # comes from GuiPrivate (see the NB above), and QWaylandWindow from WaylandClientPrivate, to reach
    # the wl_surface that blur-behind and the window shadow attach to. Neither has a public
    # equivalent. So the statement is recorded here, where it is read once, rather than reprinted as
    # ~20 lines of configure output that teach the reader to skim warnings.
    #
    # The consequence is real and belongs to packaging, not to the build: a Contour binary must be
    # run against the Qt minor release it was built against. That is why CONTOUR_QT_VALIDATED_VERSION
    # below exists and why the bundles ship their own Qt.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)

    # Gating GuiPrivate needs Qt6_VERSION, which nothing but find_package(Qt6) sets -- hence this
    # lookup, one line ahead of the gate that reads it.
    find_package(Qt6 COMPONENTS Core REQUIRED)
    set(QT_COMPONENTS Core Gui Qml Quick QuickControls2 Network Multimedia Widgets DBus)
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10)
        list(APPEND QT_COMPONENTS GuiPrivate)
    endif()

    message(STATUS "Qt components: ${QT_COMPONENTS}")
    find_package(Qt6 COMPONENTS ${QT_COMPONENTS} REQUIRED)

    # One owner of the macOS title bar's safe area: Qt::ExpandedClientAreaHint, which needs Qt 6.9 and has
    # no fallback (Step 1d, ruling D1). Checked here, inside the GUI gate, because a headless build finds
    # no Qt and its empty Qt6_VERSION would compare as older.
    if(APPLE AND Qt6_VERSION VERSION_LESS 6.9)
        message(FATAL_ERROR "macOS builds need Qt 6.9 or later (Qt::ExpandedClientAreaHint, QWindow::safeAreaMargins).")
    endif()
endif()
# }}}

# {{{ Optional features excluded from this build
# The Qt version CI builds and validates against. Building against an older Qt is supported (on macOS
# only down to Qt 6.9, which the GUI gate above enforces), but a feature needing a newer Qt — or an
# optional Qt module that is not installed — is then silently left out of the binary. Silently is the
# problem: a packager discovers it from a bug report rather than from their build log.
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

# {{{ GPU selection for Vulkan/Direct3D (renderer.gpu): QRhi::enumerateAdapters + fromRhiAdapter
# QQuickGraphicsDevice::fromRhiAdapter "was introduced in Qt 6.10". OpenGL selection (DRI_PRIME) does
# not need it and is built regardless. Linux and Windows only: macOS renders through Metal, which has
# no GPU choice to make here.
if(CONTOUR_FRONTEND_GUI)
    if(NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        contour_note_excluded_feature("GPU selection for Vulkan and Direct3D" "Linux and Windows only")
    elseif(Qt6_VERSION VERSION_GREATER_EQUAL 6.10)
        set(CONTOUR_WITH_GPU_SELECTION ON)
    else()
        contour_note_excluded_feature("GPU selection for Vulkan and Direct3D" "requires Qt 6.10")
    endif()
endif()
# }}}

# {{{ Optional: Qt's WaylandClient, for blur-behind and the window shadow
# Looked up here rather than in platform/, which is the only directory that consumes it, because
# imported targets are directory scoped: a find_package() down there leaves Qt6::WaylandClient
# undeclared in *this* scope, where the deferred qt6_finalize_target(contour) walks
# contour_platform's dependencies -- and Qt then warns, four times, that the linking might be
# incomplete. Found here, the targets are visible to platform/ (a child scope) as well.
#
# QUIET and degrading rather than REQUIRED, for the same reason as TextToSpeech above: two
# decorations must not make every Linux packager carry Qt's Wayland module. Shadowing the
# CONTOUR_WAYLAND cache option with a normal variable is how that reaches the consumer -- the guard
# in platform/CMakeLists.txt reads the shadow, because a child scope sees it in place of the cache
# entry, and the C++ side is already guarded on the matching compile definition.
if(CONTOUR_FRONTEND_GUI AND LINUX AND CONTOUR_WAYLAND)
    find_package(Qt6 COMPONENTS WaylandClient WaylandClientPrivate QUIET)
    if(NOT Qt6WaylandClient_FOUND)
        set(CONTOUR_WAYLAND OFF)
        contour_note_excluded_feature("Wayland blur & shadow" "Qt6::WaylandClient not found")
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
    CONTOUR_APP_ID="${AppId}"
)
# CONTOUR_VERSION_STRING embeds the branch name and HEAD sha, and CONTOUR_PROJECT_SOURCE_DIR is an
# absolute path that differs per worktree. Both are per-checkout values, and the compile cache keys
# on the command line: as target-wide definitions they would sit on nearly every compile and keep any
# object from being reused across worktrees, branches or commits. They are therefore attached only to
# the few sources that read them.
set(_per_checkout_definitions
    CONTOUR_VERSION_STRING="${CONTOUR_VERSION_STRING}"
    CONTOUR_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
foreach(_reader IN ITEMS
        config/Config.cpp
        cli/ContourApp.cpp)
    cmake_path(GET _reader PARENT_PATH _reader_dir)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${_reader}"
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_reader_dir}"
        APPEND PROPERTY COMPILE_DEFINITIONS ${_per_checkout_definitions})
endforeach()
set_property(SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/ContourGuiApp.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cli/ShellIntegration_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/config/Config_test.cpp"
    APPEND PROPERTY COMPILE_DEFINITIONS ${_per_checkout_definitions})
if(CONTOUR_FRONTEND_GUI)
    target_compile_definitions(contour_config PUBLIC CONTOUR_FRONTEND_GUI)
endif()
# }}}

# input/ and platform/ are GUI-only: one translates Qt events, the other adapts to Qt and the OS.
# Both sit below display/ and session/, which is why they are added before them.
if(CONTOUR_FRONTEND_GUI)
    if(APPLE)
        enable_language(OBJCXX)
    endif()
    add_subdirectory(input)
    add_subdirectory(platform)
    add_subdirectory(display)
endif()

if(CONTOUR_MACOS_APP_BUNDLE AND CONTOUR_MACOS_DEPLOY)
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
    window/ProgramStatusBadge.hpp
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
        window/AppMenuBar.hpp
        window/ContextMenuModel.hpp
        ContourGuiApp.hpp
        session/ExitCode.hpp
        session/HostedOperation.hpp
        config/LayoutBuilder.hpp
        config/LayoutStore.hpp
        session/PaneProxy.hpp
        session/ScreenshotFile.hpp
        session/SessionFactory.hpp
        window/SettingsController.hpp
        window/SettingsWindowHost.hpp
        session/TerminalSession.hpp
        session/TerminalSessionManager.hpp
        window/ChromeStyleHost.hpp
        window/ChromeTokenObjects.hpp
        window/MenuRouting.hpp
        window/SettingsWindowRules.hpp
        window/NewDefaultsNotice.hpp
        window/UiStyleProvider.hpp
        window/WindowActivation.hpp
        window/WindowControlStyleProvider.hpp
        window/WindowLivenessQt.hpp
        session/SessionInput.hpp
        session/FontControl.hpp
        session/SpawnCommand.hpp
        session/Logging.hpp
        Logging.hpp
        display/RenderState.hpp
        display/ShortcutOverride.hpp
        display/RenderingBackendSelection.hpp
        display/FirstFrameWatchdog.hpp
        display/LatencyProbeDriver.hpp
        display/GpuSelection.hpp
        display/GpuInventory.hpp
        display/DxgiGpuInventory.hpp
        display/OpenGlGpuEnvironment.hpp
        display/PciIds.hpp
        display/SysfsGpuInventory.hpp
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
        window/AppMenuBar.cpp
        window/CommandPaletteModel.cpp
        window/ContextMenuModel.cpp
        window/DaemonDriftModel.cpp
        window/NewDefaultsNotice.cpp
        ContourGuiApp.cpp
        session/PaneProxy.cpp
        config/LayoutBuilder.cpp
        config/LayoutStore.cpp
        session/SessionFactory.cpp
        window/SettingsController.cpp
        window/SettingsWindowHost.cpp
        session/TerminalSession.cpp
        display/TerminalAccessible.cpp
        display/FirstFrameWatchdog.cpp
        display/LatencyProbeDriver.cpp
        display/GpuInventory.cpp
        display/PciIds.cpp
        display/SysfsGpuInventory.cpp
        session/TerminalSessionManager.cpp
        window/PopupShadowImageProvider.cpp
        window/ChromeStyleHost.cpp
        window/ChromeTokenObjects.cpp
        window/UiStyleProvider.cpp
        window/WindowControlStyleProvider.cpp
        window/WindowController.cpp
        window/WindowPositionStore.cpp
        window/SettingsWindowRules.cpp
        session/SessionInput.cpp
        session/FontControl.cpp
        session/SpawnCommand.cpp
        remote/NativeController.cpp
        remote/RemoteController.cpp
        remote/RemoteLayout.cpp
        remote/TmuxController.cpp
    )
    if(WIN32)
        list(APPEND _core_source_files display/DxgiGpuInventory.cpp)
    endif()
endif()

if(CONTOUR_WITH_GPU_SELECTION)
    list(APPEND _core_source_files display/GraphicsDeviceSelector.cpp)
    list(APPEND _header_files display/GraphicsDeviceSelector.hpp)
endif()

if(WIN32)
    # The application manifest, which makes contour.exe's process code page UTF-8 (see the file). A
    # .manifest source is embedded by the linker, and qt_add_executable() then generates none of its own.
    # These sources are the contour executable's alone, so no test binary gets it.
    list(APPEND _main_source_files packaging/contour.rc packaging/contour.manifest)
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
        qml/ThemedWindow.qml
        qml/BellSound.qml
        qml/DeferredBell.qml
        qml/RequestPermission.qml
        qml/SearchBar.qml
        qml/SessionChrome.qml
        qml/TitleBar.qml
        qml/HostChip.qml
        qml/ToolbarButton.qml
        qml/TabStrip.qml
        qml/TabItem.qml
        qml/TabBadgeSlot.qml
        qml/TabColorFlyout.qml
        qml/WindowControls.qml
        qml/ResizeBorder.qml
        qml/PaneNode.qml
        qml/PaneCard.qml
        qml/PaneHeader.qml
        qml/TerminalPane.qml
        qml/ActionContextMenu.qml
        qml/MenuSwatchRow.qml
        qml/CommandPalette.qml
        qml/SaveLayoutDialog.qml
        qml/SettingsPage.qml
        qml/SettingsWindow.qml
        qml/SettingsNavItem.qml
        qml/SettingsListItem.qml
        qml/ConfirmDialog.qml
        qml/DaemonDriftBadge.qml
        qml/ContourMenu.qml
        qml/PopupSurface.qml
        qml/SettingRow.qml
        qml/SettingsSection.qml
        qml/ColorSchemeEditor.qml
        qml/StatusLineIndicatorEditor.qml
        qml/LinkPatternList.qml
        qml/StatusLineSegmentRow.qml
        qml/StatusLineChip.qml
        qml/StatusLineItemDialog.qml
        qml/StatusLinePreviewItem.qml
        qml/ColorSlotField.qml
        qml/Banner.qml
        qml/Toast.qml
        qml/ElevationShadow.qml
        qml/KitDialog.qml
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

# {{{ contour_command_output: the private files that hand a command's output to a pager
# Qt-free, like config/, command/ and cli/, so that its tests run in a build without the GUI frontend too.
add_library(contour_command_output STATIC
    session/CommandOutputFiles.cpp
    session/CommandOutputFiles.hpp
)
target_link_libraries(contour_command_output PUBLIC core::base core::log core::platform vtpty)
target_include_directories(contour_command_output PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# }}}

# {{{ contour_block_tooltip: what the gutter's block tooltip says about a command
# Qt-free for the same reason, and tested in contour_test rather than contour_gui_test: what proves it
# needs no Qt is a test binary that has none.
add_library(contour_block_tooltip STATIC
    session/CommandBlockTooltip.cpp
    session/CommandBlockTooltip.hpp
)
target_link_libraries(contour_block_tooltip PUBLIC vtbackend)
target_include_directories(contour_block_tooltip PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# }}}

# {{{ contour_local_file: what following a hyperlink to a file on this machine does with it
# Qt-free, and tested in contour_test, for the reason contour_block_tooltip is.
add_library(contour_local_file STATIC
    session/LocalFileAction.cpp
    session/LocalFileAction.hpp
)
target_link_libraries(contour_local_file PUBLIC core::base core::platform)
target_include_directories(contour_local_file PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# }}}

# {{{ contour_extensions: a profile's status-line extensions, as the extension runtime takes them
# Qt-free, and tested in contour_test: the configuration-to-declaration mapping, which both the GUI's
# sessions and `contour daemon` build their extensions from. The runtime itself is vtextension's, below
# both. Exists only with CONTOUR_EXTENSIONS; vtextension's PUBLIC CONTOUR_EXTENSIONS definition is what the
# sessions test for.
if(CONTOUR_EXTENSIONS)
    add_library(contour_extensions STATIC
        config/ExtensionDeclarations.cpp
        config/ExtensionDeclarations.hpp
    )
    target_link_libraries(contour_extensions PUBLIC contour_config vtextension)
    target_include_directories(contour_extensions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
# }}}

# {{{ contour_core: the reusable object library (everything but main.cpp)
add_library(contour_core OBJECT)
target_sources(contour_core PRIVATE ${_core_source_files} ${_header_files} ${_qt_resources})

# Both the binary dir (for generated headers) and the parent src dir (so "#include <contour/...>"
# resolves) must be PUBLIC so consumers of the object library inherit them.
target_include_directories(contour_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
# DXGI enumerates the GPUs for the settings page (display/DxgiGpuInventory.cpp).
if(WIN32)
    target_link_libraries(contour_core PUBLIC dxgi)
endif()
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

# LLVM 22's ThreadSanitizer runtime does not intercept glibc's pthread_clockjoin_np, the only join Qt
# 6.10's QThread::wait() uses on Linux, so it takes every waited-for QThread to be still running and
# aborts once glibc reuses its descriptor: platform/ThreadSanitizerJoin.cpp explains it and forwards the
# join to one the runtime sees. One object library for both executables that load Qt, this one and
# contour_gui_test, built only with ThreadSanitizer against glibc. The definition must be exported for
# the dynamic linker to bind Qt's call to it.
if(CONTOUR_FRONTEND_GUI AND CONTOUR_SANITIZE STREQUAL "thread")
    include(CheckSymbolExists)
    check_symbol_exists(__GLIBC__ "features.h" CONTOUR_HAVE_GLIBC)
    if(CONTOUR_HAVE_GLIBC)
        add_library(contour_tsan_thread_join OBJECT platform/ThreadSanitizerJoin.cpp)
        target_link_options(contour_tsan_thread_join INTERFACE "LINKER:--export-dynamic-symbol=pthread_clockjoin_np")
        target_link_libraries(contour PRIVATE contour_tsan_thread_join)
    endif()
endif()

# Everything Qt about these two targets, in one place. moc and rcc are Qt tools and without the GUI
# frontend there is nothing for either to do: every Q_OBJECT in this directory belongs to a GUI-only
# translation unit, and resources.qrc is not compiled above. Leaving them on would merely ask a
# Qt-free build for Qt's code generators.
if(CONTOUR_FRONTEND_GUI)
    set_target_properties(contour_core contour PROPERTIES AUTOMOC ON AUTORCC ON)
    # Disable all deprecated Qt functions prior to Qt 6.0.
    target_compile_definitions(contour_core PUBLIC QT_DISABLE_DEPRECATED_BEFORE=0x050F00)
    if(CONTOUR_WITH_GPU_SELECTION)
        target_compile_definitions(contour_core PUBLIC CONTOUR_WITH_GPU_SELECTION)
    endif()
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
    contour::tracy
    contour_block_tooltip
    contour_command
    contour_command_output
    contour_cli
    contour_local_file
    vtworkspace
    # remote/ runs the daemon client on core-cpp's event loop, session/ writes command output through
    # core::platform's filesystem, and main.cpp suppresses the Windows error dialogs through
    # core::testing, as the test binaries do.
    core::async
    core::base
    core::cli
    core::log
    core::net
    core::platform
    core::testing
    PRIVATE
    vthost
)
if(CONTOUR_EXTENSIONS)
    target_link_libraries(contour_core PUBLIC contour_extensions)
endif()
# ContourGuiApp is a composition root: it creates the WebAssembly runtime backend (makeDefaultBackend()).
if(CONTOUR_EXTENSIONS AND CONTOUR_FRONTEND_GUI)
    target_link_libraries(contour_core PRIVATE vtextension_runtime)
endif()

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
            remote/RemoteController_test.cpp
            platform/AppearanceSources_test.cpp
            platform/AudioNote_test.cpp
            platform/GuiTheme_test.cpp
            platform/MultimediaEnvironment_test.cpp
            test/BenignQtMessages_test.cpp
            config/GpuSelector_test.cpp
            test/Accessibility_qml_test.cpp
            platform/SpeechSynthesizer_test.cpp
            display/ContentScale_test.cpp
            session/ExitCode_test.cpp
            display/CaretReportGate_test.cpp
            session/HyperlinkTooltip_test.cpp
            input/HorizontalWheelGesture_test.cpp
            display/ImeQueryRect_test.cpp
            display/ShortcutOverride_test.cpp
            display/ViewportTextIndex_test.cpp
            session/KeyboardRouting_test.cpp
            test/MainWindowQml_test.cpp
            test/MainWindowControllerMock.hpp
            test/MockMainController.hpp
            test/QmlMocks.hpp
            platform/FreeDesktopNotifier_test.cpp
            platform/NotificationRouter_test.cpp
            platform/Notifier_test.cpp
            platform/ExternalLauncher_test.cpp
            platform/PortalExternalLauncher_test.cpp
            platform/PortalNotificationTransport_test.cpp
            platform/TaskbarProgressRouter_test.cpp
            platform/LauncherProgress_test.cpp
            platform/LauncherProgressSink_test.cpp
            platform/NativeWindowFrame_test.cpp
            window/WindowPositionStore_test.cpp
            window/AppMenuBar_test.cpp
            window/ContextMenuModel_test.cpp
            window/MenuRouting_test.cpp
            window/SettingsWindowRules_test.cpp
            platform/PopupShadow_test.cpp
            platform/QtInvoke_test.cpp
            platform/WindowShadowController_test.cpp
            platform/WindowShadowGeometry_test.cpp
            platform/WindowShadowTiles_test.cpp
            test/QmlDiagnosticPredicate_test.cpp
            test/ThreadJoin_test.cpp
            display/RenderingBackendSelection_test.cpp
            display/FirstFrameWatchdog_test.cpp
            display/ShaderConfig_test.cpp
            display/GpuSelection_test.cpp
            display/GpuInventory_test.cpp
            display/OpenGlGpuEnvironment_test.cpp
            display/ScissorRect_test.cpp
            display/RhiVertexLayout_test.cpp
            display/ScreenshotReadback_test.cpp
            display/ScreenshotRhiReadback_test.cpp
            window/TabColorScheme_test.cpp
            test/ContourTuiStyle_test.cpp
            window/WindowControlStyleProvider_test.cpp
            window/TabLabel_test.cpp
            window/DaemonDriftModel_test.cpp
            window/NewDefaultsNotice_test.cpp
            geometry/CellRectangle_test.cpp
            geometry/WindowGeometry_test.cpp
            platform/TitleBandMaterial_test.cpp
            platform/WindowResizeGrid_test.cpp
        )
        set_target_properties(contour_gui_test PROPERTIES AUTOMOC ON)
        if(APPLE)
            target_sources(contour_gui_test PRIVATE
                platform/CocoaAppearanceSources_test.mm
                platform/CocoaMenuBarIntegration_test.mm
                platform/CocoaNativeMenuPresenter_test.mm
                platform/CocoaTaskbarProgressSink_test.mm
                platform/CocoaTitleBandMaterial_test.mm
                platform/CocoaWindowFrame_test.mm
                platform/CocoaWindowResizeGrid_test.mm)
            set_target_properties(contour_gui_test PROPERTIES
                OBJCXX_STANDARD ${CMAKE_CXX_STANDARD}
                OBJCXX_STANDARD_REQUIRED ON)
            set_source_files_properties(
                platform/CocoaAppearanceSources_test.mm platform/CocoaMenuBarIntegration_test.mm
                platform/CocoaNativeMenuPresenter_test.mm platform/CocoaTaskbarProgressSink_test.mm
                platform/CocoaTitleBandMaterial_test.mm platform/CocoaWindowFrame_test.mm
                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        endif()
        if(CONTOUR_WITH_GPU_SELECTION)
            target_sources(contour_gui_test PRIVATE display/GraphicsDeviceSelector_test.cpp)
        endif()

        # The suite's heavyweight sources: every one whose compile peaks at 2 GB or more with GCC at -O2 -g, the
        # flags of CI's RelWithDebInfo builds. Measured in October 2026 with GCC 16.2, as the compiler's peak
        # resident set compiling each of the suite's sources alone: Config_test 7.96 GB, TerminalSession_test
        # 4.15 GB, QmlComponents_test 3.32 GB, the others listed here 2.13 to 2.99 GB -- what the frontend's
        # headers cost every source that includes them -- and every source not listed 1.73 GB or less. One at a
        # time, beside three of the light ones, a 4-job build stays under 16 GB. To re-measure, compile each
        # source alone with the flags compile_commands.json gives it, -O2 -g in place of the optimisation
        # flags, and read the compiler's peak (wait4's ru_maxrss).
        #
        # They compile in an object library of their own, in contour_heavy_compile_pool (see the top-level
        # CMakeLists.txt), as a pool is a target property before CMake 4.2 and CI has 3.31. A source listed
        # here is not listed in the suite's own sources.
        set(contour_gui_test_heavy_sources
            config/Config_test.cpp
            display/DisplayRendering_test.cpp
            display/TerminalAccessible_test.cpp
            remote/NativeController_test.cpp
            remote/TmuxController_test.cpp
            session/FocusRouting_test.cpp
            session/FontControl_test.cpp
            session/SessionInput_test.cpp
            session/TabSizeInheritance_test.cpp
            session/TerminalSessionManager_test.cpp
            session/TerminalSession_test.cpp
            test/ChromeSnapshot_test.cpp
            test/ContourGuiApp_test.cpp
            test/QmlComponents_test.cpp
            test/SettingsPageQml_test.cpp
            window/CommandPaletteModel_test.cpp
            window/MultiWindow_test.cpp
            window/SettingsController_test.cpp
            window/SettingsWindowHost_test.cpp
            window/SettingsWindowRouting_test.cpp
            window/TabListModel_test.cpp
            window/TitleBandController_test.cpp
            window/UiStyleProvider_test.cpp
            window/WindowMenus_test.cpp
            window/WindowSizing_test.cpp
        )
        if(CONTOUR_EXTENSIONS)
            list(APPEND contour_gui_test_heavy_sources session/TerminalSessionExtensions_test.cpp)
        endif()
        add_library(contour_gui_test_heavy OBJECT ${contour_gui_test_heavy_sources})
        # The chrome snapshots load the bundled fonts from the source tree, in every build type.
        target_compile_definitions(contour_gui_test_heavy PRIVATE CONTOUR_GUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        # The end-to-end permission test runs a guest that runs a command; it exists where guests are built.
        if(CONTOUR_EXTENSIONS AND TARGET spawner_wasm)
            target_compile_definitions(contour_gui_test_heavy PRIVATE
                CONTOUR_TEST_SPAWNER_WASM="${CONTOUR_CARGO_TARGET_DIR}/${CONTOUR_WASM_TARGET}/release/spawner.wasm")
            add_dependencies(contour_gui_test_heavy spawner_wasm)
        endif()
        # The font test runs the echo guest, which describes the events it follows.
        if(CONTOUR_EXTENSIONS AND TARGET echo_wasm)
            target_compile_definitions(contour_gui_test_heavy PRIVATE
                CONTOUR_TEST_ECHO_WASM="${CONTOUR_CARGO_TARGET_DIR}/${CONTOUR_WASM_TARGET}/release/echo.wasm")
            add_dependencies(contour_gui_test_heavy echo_wasm)
        endif()
        if(CMAKE_GENERATOR MATCHES "Ninja")
            set_target_properties(contour_gui_test_heavy PROPERTIES JOB_POOL_COMPILE contour_heavy_compile_pool)
        endif()

        # Both are built alike: the same usage requirements, include path, options and moc.
        foreach(target IN ITEMS contour_gui_test contour_gui_test_heavy)
            set_target_properties(${target} PROPERTIES AUTOMOC ON)
            target_link_libraries(${target}
                PRIVATE
                contour_core
                core::net_tls # remote/NativeController_test serves TLS
                Qt6::Test
                Qt6::GuiPrivate # rhi/qrhi.h + qrhigles2_p.h for the end-to-end screenshot readback test
                Catch2::Catch2
            )
            target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
            if(NOT WIN32)
                target_compile_options(${target} PRIVATE -Wno-c2y-extensions)
            endif()
        endforeach()
        target_link_libraries(contour_gui_test PRIVATE contour_gui_test_heavy)

        # The QThread join forwarding, as for the executable itself (see contour_tsan_thread_join).
        if(TARGET contour_tsan_thread_join)
            target_link_libraries(contour_gui_test PRIVATE contour_tsan_thread_join)
        endif()

        # Windows found neither Qt6Test, the one Qt library `contour` does not link, nor the
        # offscreen QPA the registration below selects, so windeployqt's staging for the application
        # covers neither and the suite died in the loader before main(). The Windows plugin as well,
        # so that running the executable by hand needs no more than having built it.
        include(StageRuntimeDeps)
        contour_stage_runtime_dlls(contour_gui_test)
        contour_stage_qt_plugins(contour_gui_test platforms
                                 Qt6::QOffscreenIntegrationPlugin
                                 Qt6::QWindowsIntegrationPlugin)

        add_test(NAME contour_gui_test COMMAND $<TARGET_FILE:contour_gui_test>)

        # The offscreen QPA, because this suite constructs a QGuiApplication and its QML trees: without
        # it the run needs a real display and fails wherever there is none. It was passing only because
        # every invocation that mattered happened to export QT_QPA_PLATFORM by hand.
        #
        # LeakSanitizer is off here for the same, already-documented reason as the e2e app runs below:
        # constructing a QGuiApplication initializes freetype through Qt's font database, whose one-time
        # global allocations are freed by teardown hooks that run AFTER LSan reports. Every block in that
        # report is freetype/Qt-internal with ZERO first-party frames, and it fires after Catch2 has
        # already reported the suite green -- so it turns a passing run into a failing test. ASan proper
        # (use-after-free, overflow) stays fully enabled; the Qt-free unit suites keep leak checking.
        #
        # A QML disk cache of this build tree's own. Qt's default is one per user and application name,
        # so every contour_gui_test on the machine -- across worktrees and build types -- shares one
        # cache, and a debug Qt has been seen to trip a V4 interpreter assert on the first run after a
        # fresh build (QTBUG-111385). And stderr for Qt's messages: on Windows they otherwise go to the
        # debugger, so a Qt assert that aborts a run under ctest names nothing.
        set(CONTOUR_QML_TEST_ENVIRONMENT
            "QML_DISK_CACHE_PATH=${CMAKE_CURRENT_BINARY_DIR}/qmlcache;QT_FORCE_STDERR_LOGGING=1")
        set_tests_properties(contour_gui_test
            PROPERTIES
                ENVIRONMENT
                    "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=detect_leaks=0;${CONTOUR_QML_TEST_ENVIRONMENT}")

        if(WIN32)
            # Native HWND geometry cannot be exercised through the offscreen platform. Keep the
            # regular suite headless, and run the native cases separately on the Windows desktop.
            add_test(NAME contour_windows_native_test
                     COMMAND $<TARGET_FILE:contour_gui_test> "[native]")
            set_tests_properties(contour_windows_native_test
                PROPERTIES
                    ENVIRONMENT
                        "CONTOUR_TEST_DISPLAY=1;QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=software;${CONTOUR_QML_TEST_ENVIRONMENT}"
                    RUN_SERIAL TRUE
                    TIMEOUT 120
                    LABELS "native;windows")
        endif()

        # Under ThreadSanitizer, ignore_noninstrumented_modules=1, for this suite only.
        #
        # Why: the distro Qt (and the glib and libdbus under it) is not instrumented, so its allocator and
        # teardown traffic shows as races the sanitizer cannot order. Qt hands memory between its threads
        # through synchronization it cannot see -- the futex-based QMutex and QSemaphore, QObject's
        # connection locks -- so a QDBusConnection hooking a receiver's destroyed() from its own thread,
        # the QML type loader's thread, the thread pool and QPainter's parallel span fills all read as
        # races. The option drops the memory accesses of interceptors those libraries call (malloc, free,
        # memcpy, ...) and keeps their mutex and condition-variable synchronization; Contour's own
        # accesses stay checked, in QThread::run(), QThreadPool tasks and queued slots alike.
        #
        # The blind spot: code that runs inside an interceptor called from uninstrumented code runs
        # unchecked -- the bodies of std::call_once and std::async through libstdc++, pthread_once and
        # qsort callbacks. A race planted in a std::call_once body or a std::async task goes unreported
        # here; one planted in QThread::run(), a QThreadPool task or a queued slot does not.
        #
        # So src/contour must not lean on this suite alone for races in such code: threaded logic that
        # runs in such bodies -- FontLocatorProvider's and SystemdScopePlacement's std::call_once, the
        # libssh2 initialisation in SshSession -- must have its races tested in an unsilenced module suite
        # (text_shaper_test, vtpty_test), not through contour_gui_test.
        set(CONTOUR_QML_TEST_TSAN_OPTIONS "TSAN_OPTIONS=string_append: ignore_noninstrumented_modules=1")
        if(CONTOUR_SANITIZE STREQUAL "thread")
            set_property(TEST contour_gui_test APPEND PROPERTY ENVIRONMENT_MODIFICATION
                         "${CONTOUR_QML_TEST_TSAN_OPTIONS}")
        endif()

        # The notification suite again, but against a session bus whose org.freedesktop.Notifications
        # is activatable and never answers -- a desktop with its notification daemon disabled, which
        # is what issue #2051 was reported from. It asserts a time budget rather than an output,
        # because the bug was not a wrong result but a 25-second wait per terminal session, paid on
        # the startup path before any window was mapped. Nothing else in the suite would notice a
        # synchronous D-Bus call coming back.
        #
        # Guarded on dbus-daemon: without it there is no way to stand up a wedged bus, and a machine
        # that lacks it simply does not run this one.
        find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon)
        if(DBUS_DAEMON_EXECUTABLE AND NOT WIN32 AND NOT APPLE)
            add_test(NAME contour_notification_nonblocking
                     COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/notification-nonblocking.sh
                             $<TARGET_FILE:contour_gui_test>)
            set_tests_properties(contour_notification_nonblocking
                PROPERTIES
                    ENVIRONMENT
                        "QT_QPA_PLATFORM=offscreen;LSAN_OPTIONS=detect_leaks=0;${CONTOUR_QML_TEST_ENVIRONMENT}"
                    TIMEOUT 120
                    LABELS e2e)
            if(CONTOUR_SANITIZE STREQUAL "thread")
                set_property(TEST contour_notification_nonblocking APPEND PROPERTY ENVIRONMENT_MODIFICATION
                             "${CONTOUR_QML_TEST_TSAN_OPTIONS}")
            endif()
        else()
            message(STATUS "dbus-daemon not found: skipping contour_notification_nonblocking test")
        endif()

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
                        daemon-mode in_process early-exit-threshold 0 execute sh -c "exit 0")
            add_test(NAME contour_e2e_vt_stream
                COMMAND $<TARGET_FILE:contour>
                        daemon-mode in_process early-exit-threshold 0
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
            # A client whose attach fails returns from the GUI verb before the event loop runs, with the QML
            # engine already built; it must still exit cleanly, the engine gone before QApplication.
            add_test(NAME contour_e2e_client_unreachable
                COMMAND setsid -w sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/client-unreachable.sh $<TARGET_FILE:contour>)
            # Where a launch hosts its sessions (`daemon.mode`): `required` ends it before any window, `auto`
            # falls back to in-process saying why, and `contour client --window` names a window the daemon
            # lacks. The default `in_process` is what every other app run here already covers.
            add_test(NAME contour_e2e_hosting
                COMMAND setsid -w sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/hosting.sh $<TARGET_FILE:contour>)
            # `contour daemon --stdio`, the remote end of an ssh attach: stdout carries its preamble and
            # nothing else, not even a warning the configuration logs before logging is installed.
            add_test(NAME contour_e2e_stdio_relay
                COMMAND setsid -w sh ${CMAKE_CURRENT_SOURCE_DIR}/test/e2e/stdio-relay.sh $<TARGET_FILE:contour>)

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
                contour_e2e_custom_config contour_e2e_cli contour_e2e_daemon contour_e2e_client_unreachable
                contour_e2e_hosting contour_e2e_stdio_relay
                PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LIBGL_ALWAYS_SOFTWARE=1;LSAN_OPTIONS=detect_leaks=0"
                    TIMEOUT 120
                    RUN_SERIAL TRUE
                    LABELS e2e)
            # Also selectable with the daemon suites: `ctest -L daemon` must cover the feature
            # end to end, not just its unit layers.
            set_property(TEST contour_e2e_daemon contour_e2e_client_unreachable contour_e2e_hosting
                              contour_e2e_stdio_relay
                         APPEND PROPERTY LABELS daemon)

            # Under ThreadSanitizer, the same ignore_noninstrumented_modules=1 as contour_gui_test, and
            # for the same reason: the application loads the same uninstrumented Qt, glib and libdbus,
            # plus Qt Multimedia's PulseAudio, whose allocator and teardown traffic between their own
            # threads shows as races the sanitizer cannot order. Contour's own accesses stay checked.
            # The same blind spot applies -- std::call_once and std::async bodies through libstdc++,
            # pthread_once and qsort callbacks run unchecked -- with the same consequence: threaded logic
            # in such bodies has its races tested in an unsilenced module suite, not through these runs.
            if(CONTOUR_SANITIZE STREQUAL "thread")
                set_property(TEST contour_e2e_lifecycle contour_e2e_vt_stream contour_e2e_custom_config
                                  contour_e2e_cli contour_e2e_daemon contour_e2e_client_unreachable
                                  contour_e2e_hosting contour_e2e_stdio_relay
                             APPEND PROPERTY ENVIRONMENT_MODIFICATION "${CONTOUR_QML_TEST_TSAN_OPTIONS}")
            endif()
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
        config/ChromeTokenGroups_test.cpp
        config/FileOpenerTemplate_test.cpp
        config/DaemonSettings_test.cpp
        config/GlobalOverrides_test.cpp
        config/GuiStateStore_test.cpp
        config/HostUrl_test.cpp
        config/LayoutBuilder_test.cpp
        config/NewDefaultsOffer_test.cpp
        config/PtyReadBufferSize_test.cpp
        config/TabBarMode_test.cpp
        config/UiStyle_test.cpp
        config/WindowControlStyle_test.cpp
        test/ChromeSnapshotPlan_test.cpp
        command/CommandCatalog_test.cpp
        command/CommandHistory_test.cpp
        command/ContextMenu_test.cpp
        command/FuzzyFilter_test.cpp
        command/MenuBarTable_test.cpp
        command/MenuTree_test.cpp
        command/Shortcut_test.cpp
        command/TabMenus_test.cpp
        command/TitleBarContextMenu_test.cpp
        cli/AgentSkill_test.cpp
        cli/CaptureScreen_test.cpp
        cli/ContourApp_test.cpp
        cli/ControlVerbs_test.cpp
        cli/DaemonVerbs_test.cpp
        cli/Hosting_test.cpp
        cli/HostTransport_test.cpp
        cli/CatImageArgs_test.cpp
        cli/ConfigProfileCatalog_test.cpp
        cli/ConfigReloadPoller_test.cpp
        cli/ShellIntegration_test.cpp
        cli/ShellIntegrationShells_test.cpp
        display/RedrawCoalescer_test.cpp
        session/SearchStatus_test.cpp
        session/ScreenshotFile_test.cpp
        session/CommandOutputFiles_test.cpp
        session/CommandBlockTooltip_test.cpp
        session/LocalFileAction_test.cpp
        session/EndReason_test.cpp
        platform/FinishNotification_test.cpp
        platform/NotificationOccasion_test.cpp
        platform/SessionVisibility_test.cpp
        window/HeldModifierReveal_test.cpp
        window/PaneHeaderPolicy_test.cpp
        window/PaneState_test.cpp
        window/SplitChrome_test.cpp
        window/ProgramStatusBadge_test.cpp
        window/RemoteChrome_test.cpp
        window/HostChipState_test.cpp
        window/TabAttention_test.cpp
        window/TabBadge_test.cpp
        window/TabWellLayout_test.cpp
        window/TitleBandLayout_test.cpp
        window/TitleBandSurface_test.cpp
        window/TitleBandGeometry_test.cpp
    )
    # vtbackend, deliberately: SearchStatus is a pure decision over vtbackend's SearchMatchTally,
    # ScreenshotFile one over its Screen, and FinishNotification one over its CommandBlockSummary; testing
    # them HERE rather than in contour_gui_test is what proves they need no Qt to reach.
    target_link_libraries(contour_test
        PRIVATE
        contour_block_tooltip
        contour_command
        contour_command_output
        contour_cli
        contour_local_file
        vthost
        vtcontrol
        glaze::glaze
        vtbackend
        core::testing
        Catch2::Catch2)
    target_include_directories(contour_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    # `contour api wit` must print the checked-in WIT package, and the exit statuses the docs list must be the
    # skill's; the tests compare against the files themselves.
    target_compile_definitions(contour_test PRIVATE
        CONTOUR_CONTROL_WIT="${PROJECT_SOURCE_DIR}/wit/control/world.wit"
        CONTOUR_CONTROL_API_DOC="${PROJECT_SOURCE_DIR}/docs/control-api.md")
    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(contour_test PRIVATE -Wno-c2y-extensions)
    endif()
    if(CONTOUR_EXTENSIONS)
        target_sources(contour_test PRIVATE
            config/ExtensionDeclarations_test.cpp
        )
        target_link_libraries(contour_test PRIVATE contour_extensions)
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
    # The shells travel comma-separated: a ';' would split the list into arguments of its own on the
    # way through ARGS and add_test, leaving SHELLS the first shell alone.
    string(REPLACE ";" "," _shell_integration_shells "${CONTOUR_SHELL_INTEGRATION_SHELLS}")
    contour_add_e2e_cmake_test(
        NAME contour_e2e_shell_integration
        SCRIPT ShellIntegration.cmake
        MARKER "shell-integration: OK"
        ARGS -D "SHELLS=${_shell_integration_shells}"
             -D "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cli"
             -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}")

    # The application manifest (packaging/contour.manifest), read back out of the built contour.exe as
    # Windows reads it. Its own executable, so that building the unit tests does not build the application.
    if(WIN32)
        add_executable(contour_manifest_test test/core_test_main.cpp test/ApplicationManifest_test.cpp)
        target_link_libraries(contour_manifest_test PRIVATE core::base core::testing Catch2::Catch2)
        target_compile_definitions(contour_manifest_test PRIVATE CONTOUR_BINARY="$<TARGET_FILE:contour>")
        add_dependencies(contour_manifest_test contour)
        add_test(NAME contour_e2e_manifest COMMAND $<TARGET_FILE:contour_manifest_test>)
        set_tests_properties(contour_e2e_manifest PROPERTIES LABELS e2e)
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
    set(terminfo_generate contour generate terminfo to ${terminfo_file})
    if(CONTOUR_CODE_COVERAGE_ENABLED)
        # This step runs the instrumented contour, which on exit merges its counters into the build tree's .gcda
        # files. Once a source has changed they are stale, and the coverage runtime, which trusts a stale file's
        # layout, may read past its end: the step died of SIGSEGV and failed the build. Its counters go to a
        # directory of their own instead, emptied first; this run is no coverage anyone measures.
        set(terminfo_counters "${CMAKE_CURRENT_BINARY_DIR}/terminfo-gcov")
        set(terminfo_generate
            ${CMAKE_COMMAND} -E rm -rf "${terminfo_counters}" &&
            ${CMAKE_COMMAND} -E env "GCOV_PREFIX=${terminfo_counters}"
                $<TARGET_FILE:contour> generate terminfo to ${terminfo_file})
    endif()
    add_custom_command(
        TARGET contour POST_BUILD
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND ${terminfo_generate} && ${TIC} -x -o "${terminfo_basedir}" "${terminfo_file}"
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
    if(CONTOUR_MACOS_DEPLOY)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
    endif()
endif()
# }}}

# {{{ CPACK variable definitions
if(NOT(CPACK_GENERATOR))
    # A disk image is a way to hand someone an app to drag into /Applications; without a bundle to
    # put in it there is nothing to drag, so a GUI-less macOS build falls through to CPack's own
    # default (a tarball) like any other Unix CLI tool.
    if(CONTOUR_MACOS_APP_BUNDLE AND CONTOUR_MACOS_DEPLOY)
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
if(CONTOUR_MACOS_APP_BUNDLE AND CONTOUR_MACOS_DEPLOY)
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

# {{{ Bundled status-line extensions
# Installs each bundled extension as <destination>/<name>.wasm, which is where
# vtextension::bundledExtensionDirectories() looks for it on the platform. Nothing is loaded for being
# installed: a profile names the extensions it runs.
function(contour_install_bundled_extensions destination)
    foreach(_name _module IN ZIP_LISTS CONTOUR_BUNDLED_EXTENSIONS CONTOUR_BUNDLED_EXTENSION_MODULES)
        install(FILES "${_module}" DESTINATION "${destination}" RENAME "${_name}.wasm")
    endforeach()
endfunction()
if(TARGET contour_bundled_extensions)
    # Building contour builds what installing it installs, also when only the contour target is built.
    add_dependencies(contour contour_bundled_extensions)
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
    contour_install_bundled_extensions("share/contour/extensions")
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
                # A developer benchmark, built here so that it runs beside the staged conpty.dll: the .exe,
                # and the .ilk an incremental link leaves beside it.
                PATTERN "bench-headless.*" EXCLUDE
                # The latency benchmark, likewise: the .exe and the .ilk beside it.
                PATTERN "bench-latency.*" EXCLUDE
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
    # JetBrains Mono (SIL OFL 1.1), registered with CoreText at startup. The file names contain brackets,
    # which install(DIRECTORY ... FILES_MATCHING) would read as globs, so the directory is copied whole.
    install(DIRECTORY "res/fonts/" DESTINATION "${INSTALL_CMAKE_DIR}/fonts" PATTERN "README.md" EXCLUDE)
    # Before the deployment step below, which signs the bundle with them inside.
    contour_install_bundled_extensions("${INSTALL_CMAKE_DIR}/extensions")

    # add_custom_target(Docs SOURCES README.md LICENSE.txt)
    # TODO: install(TARGETS Docs ...)

    # Destination paths below are relative to ${CMAKE_INSTALL_PREFIX}
    install(TARGETS ${APP_NAME}
        BUNDLE DESTINATION . COMPONENT Runtime
        RUNTIME DESTINATION "${INSTALL_RUNTIME_DIR}" COMPONENT Runtime
    )

    if(CONTOUR_MACOS_DEPLOY)
        include(DeployQt)
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
    endif()
else()
    # Any other Unix -- and, since the branch above now takes only bundle builds, a GUI-less macOS
    # build too. That one produces a plain command-line binary, so it installs like any other Unix
    # CLI tool: the executable and the data every platform wants, first.
    include(GNUInstallDirs)
    install(TARGETS contour DESTINATION bin)
    install(DIRECTORY "cli/shell-integration" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour")
    install(DIRECTORY "res/fonts/" DESTINATION "${CMAKE_INSTALL_DATADIR}/contour/fonts" PATTERN "README.md" EXCLUDE)
    contour_install_bundled_extensions("${CMAKE_INSTALL_DATADIR}/contour/extensions")
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
            if(CONTOUR_WITH_SYSTEMD)
                set(DEPENDS_EXTRA "${DEPENDS_EXTRA}, libsystemd0")
            endif()
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

        # A desktop launches Contour as a systemd unit of its own, named after the application ID
        # (https://systemd.io/DESKTOP_ENVIRONMENTS/), with OOMPolicy=stop: an out-of-memory kill of anything
        # in it stops Contour. These drop-ins make the unit continue instead -- one per name pattern the
        # launchers use -- see docs/internals/session-oom-isolation.md. systemd looks for user units under
        # <prefix>/lib/systemd/user, never lib64, hence not CMAKE_INSTALL_LIBDIR. Not inside Flatpak, whose
        # sandbox launches its own scope and installs nothing on the host.
        if(LINUX AND CONTOUR_WITH_SYSTEMD AND NOT EXISTS "/.flatpak-info")
            set(_systemdUserUnitDir "lib/systemd/user")
            # KDE Plasma's KIO launcher (app-<id>@<random>.service), and XDG autostart (app-<id>@autostart.service).
            set(_oomPolicyServices "app-${AppId}@.service")
            # Launchers that start scopes: KDE's scope launcher (app-<id>-<random>.scope), GNOME Shell
            # (app-gnome-<id>-<pid>.scope), and GLib's GDesktopAppInfo (app-glib-<id>-<pid>.scope).
            set(_oomPolicyScopes "app-${AppId}-.scope" "app-gnome-${AppId}-.scope" "app-glib-${AppId}-.scope")
            foreach(_unit IN LISTS _oomPolicyServices)
                install(FILES "packaging/systemd/oom-policy-service.conf"
                        DESTINATION "${_systemdUserUnitDir}/${_unit}.d" RENAME "50-oom-policy.conf")
            endforeach()
            foreach(_unit IN LISTS _oomPolicyScopes)
                install(FILES "packaging/systemd/oom-policy-scope.conf"
                        DESTINATION "${_systemdUserUnitDir}/${_unit}.d" RENAME "50-oom-policy.conf")
            endforeach()

            # The daemon that hosts persistent sessions, as a user service. Shipped, not enabled:
            # `contour daemon-service install` writes the user's own copy and enables it.
            configure_file("packaging/systemd/contour-daemon.service.in"
                           "${CMAKE_CURRENT_BINARY_DIR}/contour-daemon.service" @ONLY)
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/contour-daemon.service"
                    DESTINATION "${_systemdUserUnitDir}")
        endif()
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
