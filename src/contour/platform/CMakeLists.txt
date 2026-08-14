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
    Announcer.hpp
    Audio.cpp Audio.hpp
    AudioNote.hpp
    BlurBehind.cpp BlurBehind.hpp
    Clipboard.cpp Clipboard.hpp
    ColorConversion.hpp
    ExternalLauncher.hpp
    # Both translation units are wholly inside `#ifdef __linux__`, so they compile to nothing
    # elsewhere. Listed unconditionally all the same: AUTOMOC must still see the Q_OBJECT.
    DBusNotificationTransport.cpp DBusNotificationTransport.hpp
    FreeDesktopNotifier.cpp FreeDesktopNotifier.hpp
    GuiTheme.hpp
    NotificationRouter.hpp
    NotificationTransport.hpp
    Notifier.cpp Notifier.hpp
    QtExternalLauncher.cpp QtExternalLauncher.hpp
    QtInvoke.hpp
    QtPath.hpp
    SpeechSynthesizer.cpp SpeechSynthesizer.hpp
    TaskbarProgressRouter.hpp
    WindowShadowGeometry.hpp
    WindowShadowTiles.cpp WindowShadowTiles.hpp
    XcbProperty.cpp XcbProperty.hpp
)
set_target_properties(contour_platform PROPERTIES AUTOMOC ON)

# contour_config, because GuiTheme.hpp resolves a configured theme and ColorConversion.hpp converts the
# palette the configuration model owns.
target_link_libraries(contour_platform PUBLIC
    contour_config
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Multimedia
)

target_include_directories(contour_platform PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

if(CONTOUR_WITH_TTS)
    target_compile_definitions(contour_platform PUBLIC CONTOUR_WITH_TTS)
    target_link_libraries(contour_platform PUBLIC Qt6::TextToSpeech)
endif()

# BlurBehind.cpp is the only consumer of either: the X11 blur-behind property is set through xcb,
# and the Wayland one through the KWin blur protocol generated from protocols/blur.xml.
if(OPENBSD)
    find_package(X11 REQUIRED)
    target_link_libraries(contour_platform PUBLIC X11::xcb X11::xcb_cursor)
elseif(NOT WIN32 AND NOT APPLE)
    target_link_libraries(contour_platform PUBLIC xcb)
endif()

if(LINUX AND CONTOUR_WAYLAND)
    find_package(Qt6 COMPONENTS WaylandClient WaylandClientPrivate)
    target_compile_definitions(contour_platform PUBLIC CONTOUR_WAYLAND)
    target_link_libraries(contour_platform PUBLIC Qt6::WaylandClient Qt6::WaylandClientPrivate)
    qt6_generate_wayland_protocol_client_sources(contour_platform
                                                 FILES "${CMAKE_CURRENT_SOURCE_DIR}/protocols/blur.xml")
    # Explicitly add the generated protocol source code to the target, as the macro sometimes fails
    # to do so.
    target_sources(contour_platform PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/wayland-blur-protocol.c"
        "${CMAKE_CURRENT_BINARY_DIR}/qwayland-blur.cpp")
endif()
