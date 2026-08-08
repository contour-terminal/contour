set(_header_files
    AtlasBudget.hpp
    BackgroundRenderer.hpp
    BoxDrawingRenderer.hpp
    CursorRenderer.hpp
    DecorationRenderer.hpp
    GlyphAdvance.hpp
    GlyphScaling.hpp
    GlyphSlicing.hpp
    GridMetrics.hpp
    ImageRenderer.hpp
    Pixmap.hpp
    RenderTarget.hpp
    ReGISFontRasterizer.hpp
    Renderer.hpp
    TextClusterGrouper.hpp
    TextRenderer.hpp
    TextureAtlas.hpp
    UnderlineGeometry.hpp
    Utils.hpp
)

set(_source_files
    BackgroundRenderer.cpp
    BoxDrawingRenderer.cpp
    CursorRenderer.cpp
    DecorationRenderer.cpp
    ImageRenderer.cpp
    Pixmap.cpp
    RenderTarget.cpp
    ReGISFontRasterizer.cpp
    Renderer.cpp
    TextClusterGrouper.cpp
    TextRenderer.cpp
    Utils.cpp
)

set(_test_files
    AtlasBudget_test.cpp
    TextClusterGrouper_test.cpp
    BoxDrawingRenderer_test.cpp
    DecorationRenderer_test.cpp
    GlyphAdvance_test.cpp
    GlyphScaling_test.cpp
    GlyphSlicing_test.cpp
    ImageRenderer_test.cpp
    ReGISFontRasterizer_test.cpp
    TextRenderer_test.cpp
    UnderlineGeometry_test.cpp
)

source_group(Sources FILES ${_source_files})
source_group(Headers FILES ${_header_files})
source_group(Tests FILES ${_test_files})

add_library(vtrasterizer STATIC)
target_sources(vtrasterizer PRIVATE ${_source_files} PUBLIC ${_header_files})

target_include_directories(vtrasterizer PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vtrasterizer PUBLIC vtbackend crispy::core text_shaper)

if(CONTOUR_TESTING)
    add_executable(vtrasterizer_test)
    target_sources(vtrasterizer_test PRIVATE ${_test_files})
    target_link_libraries(vtrasterizer_test vtrasterizer Catch2::Catch2)
    add_test(NAME vtrasterizer_test COMMAND $<TARGET_FILE:vtrasterizer_test>)
    if(NOT WIN32)
        target_compile_options(vtrasterizer_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()

message(STATUS "[vtrasterizer] Compile unit tests: ${CONTOUR_TESTINGG}")
