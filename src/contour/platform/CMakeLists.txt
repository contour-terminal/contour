# Adapters to Qt and to the operating system: the bell, blur-behind, desktop notifications, speech,
# opening a URL, and the small Qt conversions everything above needs.
#
# What belongs here is anything that reaches an ambient resource -- an audio device, a D-Bus
# session, a compositor, the desktop's URL handler. Each one is split into the decision and the
# adapter (FreeDesktopNotifier vs DBusNotificationTransport, AudioNote vs Audio, speakableText vs
# QtSpeechSynthesizer), so the decision stays testable when the resource is absent.
#
# STATIC, unlike contour_core. The OBJECT form is what contour_core needs for its AUTORCC resource
# registration -- symbols nothing references by name -- and CMake does not propagate one object
# library's objects through another, so an OBJECT library here would simply never reach the binary.
# Nothing in this target is registered reflectively: Audio, Notifier and the two notification
# classes declare Q_OBJECT, but their moc output is referenced by the very code that emits their
# signals, so the archive keeps it.
add_library(contour_platform STATIC
    AccessibilityPreferenceSource.cpp AccessibilityPreferenceSource.hpp
    Announcer.hpp
    AppearanceServices.hpp
    Audio.cpp Audio.hpp
    AudioNote.hpp
    MultimediaEnvironment.hpp
    NativeMenuPresenter.cpp NativeMenuPresenter.hpp
    BlurBehind.cpp BlurBehind.hpp
    Clipboard.cpp Clipboard.hpp
    ColorConversion.hpp
    ExternalLauncher.cpp ExternalLauncher.hpp
    FinishNotification.hpp
    NotificationOccasion.hpp
    SessionVisibility.hpp
    # These translation units are wholly inside `#ifdef __linux__`, so they compile to nothing
    # elsewhere. Listed unconditionally all the same: AUTOMOC must still see the Q_OBJECT.
    DBusNotificationTransport.cpp DBusNotificationTransport.hpp
    DBusSignalSubscription.hpp
    FreeDesktopNotifier.cpp FreeDesktopNotifier.hpp
    PortalCall.cpp PortalCall.hpp
    PortalExternalLauncher.cpp PortalExternalLauncher.hpp
    PlatformAppearance.cpp PlatformAppearance.hpp
    PortalNotificationTransport.cpp PortalNotificationTransport.hpp
    GuiTheme.hpp
    NotificationIdMap.hpp
    NotificationRouter.hpp
    NotificationTransport.hpp
    Notifier.cpp Notifier.hpp
    QtExternalLauncher.cpp QtExternalLauncher.hpp
    QtInvoke.hpp
    QtPath.hpp
    SpeechSynthesizer.cpp SpeechSynthesizer.hpp
    TaskbarProgressRouter.hpp
    TaskbarProgressSink.cpp TaskbarProgressSink.hpp
    LauncherProgress.hpp LauncherProgressSink.cpp
    TaskbarProgressState.hpp
    # The per-OS window frame and the two compositor shadow adapters. Each of the platform-specific
    # ones is wholly inside a platform #ifdef, so it compiles to nothing where it does not apply --
    # listed unconditionally all the same, as the notification pair above is.
    AlphaBlur.hpp
    CocoaWindowFrame.hpp
    Dwmapi.hpp
    NativeWindowFrame.cpp NativeWindowFrame.hpp
    PopupShadow.cpp PopupShadow.hpp
    WaylandWindowShadow.cpp WaylandWindowShadow.hpp
    Win32WindowFrame.cpp Win32WindowFrame.hpp
    WindowDecoration.hpp
    WindowResizeGrid.cpp WindowResizeGrid.hpp
    WindowShadow.cpp WindowShadow.hpp
    WindowShadowController.hpp
    WindowShadowGeometry.hpp
    WindowShadowTiles.cpp WindowShadowTiles.hpp
    X11WindowShadow.cpp X11WindowShadow.hpp
    XcbProperty.cpp XcbProperty.hpp
)
set_target_properties(contour_platform PROPERTIES AUTOMOC ON)

if(WIN32)
    target_link_libraries(contour_platform PUBLIC comctl32 ole32 user32 uuid)
endif()

# The Cocoa adapters use Objective-C++; the parent enables the language for these and the native tests
# inside the APPLE guard. Enabling it on a host
# with no Objective-C compiler fails the configure step outright. The .mm sources are added the same
# way, rather than listed unconditionally like the platform .cpp files above: CMake would try to
# compile them as C++ elsewhere rather than preprocess them away.
if(APPLE)
    set_target_properties(contour_platform PROPERTIES
        OBJCXX_STANDARD ${CMAKE_CXX_STANDARD}
        OBJCXX_STANDARD_REQUIRED ON)
    target_sources(contour_platform PRIVATE
        CocoaAccessibilityPreferenceSource.mm
        CocoaPlatformAppearance.mm
        CocoaTaskbarProgressSink.mm
        CocoaWindowFrame.mm
        CocoaWindowResizeGrid.mm)
    set_source_files_properties(
        CocoaAccessibilityPreferenceSource.mm CocoaPlatformAppearance.mm CocoaTaskbarProgressSink.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(contour_platform PUBLIC "-framework AppKit")
endif()

# contour_config, because GuiTheme.hpp resolves a configured theme and ColorConversion.hpp converts the
# palette the configuration model owns; contour_command, because NativeMenuPresenter.hpp shows its
# resolved MenuTree.
target_link_libraries(contour_platform PUBLIC
    contour_command
    contour_config
    core::base
    core::log
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Quick
)

target_include_directories(contour_platform PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

if(CONTOUR_WITH_TTS)
    target_compile_definitions(contour_platform PUBLIC CONTOUR_WITH_TTS)
    target_link_libraries(contour_platform PUBLIC Qt6::TextToSpeech)
endif()

# xcb and the KWin protocols serve two features here, and only these: the blur-behind effect
# (BlurBehind.cpp, protocols/blur.xml) and the window drop shadow a client-decorated window has to
# publish for itself (X11WindowShadow.cpp and WaylandWindowShadow.cpp, protocols/shadow.xml).
if(OPENBSD)
    find_package(X11 REQUIRED)
    target_link_libraries(contour_platform PUBLIC X11::xcb X11::xcb_cursor)
elseif(NOT WIN32 AND NOT APPLE)
    target_link_libraries(contour_platform PUBLIC xcb)
endif()

# Qt6::WaylandClient{,Private} are found one directory up, in src/contour/CMakeLists.txt, so that
# they are declared in the scope that finalizes the contour executable; that gate also clears
# CONTOUR_WAYLAND when the module is absent, so reaching this block means the targets exist.
if(LINUX AND CONTOUR_WAYLAND)
    target_compile_definitions(contour_platform PUBLIC CONTOUR_WAYLAND)
    target_link_libraries(contour_platform PUBLIC Qt6::WaylandClient Qt6::WaylandClientPrivate)
    qt6_generate_wayland_protocol_client_sources(contour_platform
                                                 FILES "${CMAKE_CURRENT_SOURCE_DIR}/protocols/blur.xml"
                                                       "${CMAKE_CURRENT_SOURCE_DIR}/protocols/shadow.xml")
    # Explicitly add the generated protocol source code to the target, as the macro sometimes fails
    # to do so.
    target_sources(contour_platform PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/wayland-blur-protocol.c"
        "${CMAKE_CURRENT_BINARY_DIR}/qwayland-blur.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/wayland-shadow-protocol.c"
        "${CMAKE_CURRENT_BINARY_DIR}/qwayland-shadow.cpp")
endif()
