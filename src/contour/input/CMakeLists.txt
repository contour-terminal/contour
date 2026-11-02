# Qt event -> terminal input translation, and nothing else.
#
# This layer deliberately knows nothing about TerminalSession: what a key chord *means* is decided
# here, where it is *delivered* is decided a layer up (session/SessionInput). That is what keeps the
# platform-specific modifier and lock-key handling — three #ifdef branches deep — a pure function of
# its arguments, and therefore testable without a window, a session or the machine's live lock state.
add_library(contour_input STATIC
    HorizontalWheelGesture.hpp
    KeyEvent.hpp
    KeyMapping.cpp KeyMapping.hpp
    KeyRouter.cpp KeyRouter.hpp
    KeyTranslation.hpp
    KeyboardLayout.cpp KeyboardLayout.hpp
    KeyboardPlatform.cpp KeyboardPlatform.hpp
    Logging.hpp
    MacKeyboardPlatform.cpp
    MouseMapping.cpp MouseMapping.hpp
    NativeKeyEvent.cpp NativeKeyEvent.hpp
    QtKeyCapture.cpp QtKeyCapture.hpp
    Win32KeyboardPlatform.cpp
    XkbKeyboardPlatform.cpp
)

target_link_libraries(contour_input PUBLIC
    contour_config
    core::log
    crispy::core
    vtbackend
    Qt6::Core
    Qt6::Gui
)

target_include_directories(contour_input PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

if(UNIX AND NOT APPLE)
    # XkbLiveKeymap.cpp reads the keymap the X server serves, because the XKB keycode Qt reports is
    # positional and says nothing about the character: libxkbcommon compiles it and answers per keycode, and
    # its x11 half fetches it from the server over Qt's own xcb connection. Why this source and not Qt's own
    # or the environment's: docs/internals/keyboard-input.md.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
    pkg_check_modules(XKBCOMMON_X11 REQUIRED IMPORTED_TARGET xkbcommon-x11)
    target_sources(contour_input PRIVATE XkbLiveKeymap.cpp XkbLiveKeymap.hpp)
    target_link_libraries(contour_input PUBLIC PkgConfig::XKBCOMMON PkgConfig::XKBCOMMON_X11)
endif()

if(APPLE)
    # KeyboardLayout.cpp asks the Text Input Sources API what the active layout makes of a key
    # code, because [NSEvent keyCode] -- which is what Qt hands us -- is positional and says
    # nothing about the character. TIS* and UCKeyTranslate both live in Carbon/HIToolbox.
    find_library(CARBON_LIBRARY Carbon REQUIRED)
    target_link_libraries(contour_input PUBLIC ${CARBON_LIBRARY})
endif()
