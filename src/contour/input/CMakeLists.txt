# Qt event -> terminal input translation, and nothing else.
#
# This layer deliberately knows nothing about TerminalSession: what a key chord *means* is decided
# here, where it is *delivered* is decided a layer up (session/SessionInput). That is what keeps the
# platform-specific modifier and lock-key handling — three #ifdef branches deep — a pure function of
# its arguments, and therefore testable without a window, a session or the machine's live lock state.
add_library(contour_input STATIC
    HorizontalWheelGesture.h
    KeyMapping.cpp KeyMapping.h
    KeyboardLayout.cpp KeyboardLayout.h
    Logging.h
    MouseMapping.cpp MouseMapping.h
)

target_link_libraries(contour_input PUBLIC
    crispy::core
    vtbackend
    Qt6::Core
    Qt6::Gui
)

target_include_directories(contour_input PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

if(APPLE)
    # KeyboardLayout.cpp asks the Text Input Sources API what the active layout makes of a key
    # code, because [NSEvent keyCode] -- which is what Qt hands us -- is positional and says
    # nothing about the character. TIS* and UCKeyTranslate both live in Carbon/HIToolbox.
    find_library(CARBON_LIBRARY Carbon REQUIRED)
    target_link_libraries(contour_input PUBLIC ${CARBON_LIBRARY})
endif()
