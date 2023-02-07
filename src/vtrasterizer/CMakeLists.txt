add_library(vtrasterizer STATIC
    BackgroundRenderer.cpp BackgroundRenderer.h
    BoxDrawingRenderer.cpp BoxDrawingRenderer.h
    CursorRenderer.cpp CursorRenderer.h
    DecorationRenderer.cpp DecorationRenderer.h
    GridMetrics.h
    ImageRenderer.cpp ImageRenderer.h
    Pixmap.cpp Pixmap.h
    RenderTarget.cpp RenderTarget.h
    Renderer.cpp Renderer.h
    TextRenderer.cpp TextRenderer.h
    TextureAtlas.h
    utils.cpp utils.h
)

set_target_properties(vtrasterizer PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")

target_include_directories(vtrasterizer PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vtrasterizer PUBLIC vtbackend crispy::core text_shaper range-v3::range-v3)
