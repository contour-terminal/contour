set(_header_files
    BackgroundRenderer.h
    BoxDrawingRenderer.h
    CursorRenderer.h
    DecorationRenderer.h
    GridMetrics.h
    ImageRenderer.h
    Pixmap.h
    RenderTarget.h
    Renderer.h
    TextClusterGrouper.h
    TextRenderer.h
    TextureAtlas.h
    utils.h
)

set(_source_files
    BackgroundRenderer.cpp
    BoxDrawingRenderer.cpp
    CursorRenderer.cpp
    DecorationRenderer.cpp
    ImageRenderer.cpp
    Pixmap.cpp
    RenderTarget.cpp
    Renderer.cpp
    TextClusterGrouper.cpp
    TextRenderer.cpp
    utils.cpp
)

set(_test_files
    TextClusterGrouper_test.cpp
)

source_group(Sources FILES ${_source_files})
source_group(Headers FILES ${_header_files})
source_group(Tests FILES ${_test_files})

add_library(vtrasterizer STATIC)
target_sources(vtrasterizer PRIVATE ${_source_files} PUBLIC ${_header_files})

set_target_properties(vtrasterizer PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")

target_include_directories(vtrasterizer PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vtrasterizer PUBLIC vtbackend crispy::core text_shaper)

if(CONTOUR_TESTING)
    enable_testing()
    add_executable(vtrasterizer_test)
    target_sources(vtrasterizer_test PRIVATE ${_test_files})
    target_link_libraries(vtrasterizer_test vtrasterizer Catch2::Catch2WithMain)
    add_test(vtrasterizer_test ./vtrasterizer_test)
endif()

message(STATUS "[vtrasterizer] Compile unit tests: ${CONTOUR_TESTINGG}")
