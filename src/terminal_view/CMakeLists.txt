find_package(OpenGL REQUIRED)
find_package(Qt5 COMPONENTS Gui REQUIRED)  # apt install qtbase5-dev libqt5gui5

CIncludeMe(shaders/background.frag "${CMAKE_CURRENT_BINARY_DIR}/background_frag.h" "background_frag" "default_shaders")
CIncludeMe(shaders/background.vert "${CMAKE_CURRENT_BINARY_DIR}/background_vert.h" "background_vert" "default_shaders")
CIncludeMe(shaders/text.frag "${CMAKE_CURRENT_BINARY_DIR}/text_frag.h" "text_frag" "default_shaders")
CIncludeMe(shaders/text.vert "${CMAKE_CURRENT_BINARY_DIR}/text_vert.h" "text_vert" "default_shaders")

add_library(terminal_view STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/background_frag.h"
    "${CMAKE_CURRENT_BINARY_DIR}/background_vert.h"
    "${CMAKE_CURRENT_BINARY_DIR}/text_frag.h"
    "${CMAKE_CURRENT_BINARY_DIR}/text_vert.h"
    BackgroundRenderer.cpp BackgroundRenderer.h
    CursorRenderer.cpp CursorRenderer.h
    DecorationRenderer.cpp DecorationRenderer.h
    GridMetrics.h
    ImageRenderer.cpp ImageRenderer.h
    OpenGLRenderer.cpp OpenGLRenderer.h
    Renderer.cpp Renderer.h
    ShaderConfig.cpp ShaderConfig.h
    TerminalView.cpp TerminalView.h
    TextRenderer.cpp TextRenderer.h
)

target_include_directories(terminal_view PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(terminal_view PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

set(TERMINAL_VIEW_LIBRARIES Qt5::Gui terminal crispy::core text_shaper)

target_link_libraries(terminal_view PUBLIC ${TERMINAL_VIEW_LIBRARIES})

