add_library(terminal_renderer STATIC
    Atlas.cpp Atlas.h
    BackgroundRenderer.cpp BackgroundRenderer.h
    BoxDrawingRenderer.cpp BoxDrawingRenderer.h
    CursorRenderer.cpp CursorRenderer.h
    DecorationRenderer.cpp DecorationRenderer.h
    GridMetrics.h
    ImageRenderer.cpp ImageRenderer.h
    Pixmap.cpp Pixmap.h
    Renderer.cpp Renderer.h
    TextRenderer.cpp TextRenderer.h
    utils.cpp utils.h
)

target_include_directories(terminal_renderer PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(terminal_renderer PUBLIC terminal crispy::core text_shaper range-v3)
