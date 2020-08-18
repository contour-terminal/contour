find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Qt5 COMPONENTS Gui REQUIRED)  # apt install qtbase5-dev libqt5gui5

if(APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(fontconfig REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
    execute_process(
        COMMAND sh -c "brew --prefix harfbuzz | cut -d. -f1 | tr -d $'\n'"
        OUTPUT_VARIABLE HARFBUZZ_APPLE_INCLUDE)
    include_directories(${HARFBUZZ_APPLE_INCLUDE}/include)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux")
      #find_package(fontconfig)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    #find_package(unofficial-fontconfig)
    find_package(harfbuzz CONFIG REQUIRED)
endif()

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
    ImageRenderer.cpp ImageRenderer.h
    OpenGLRenderer.cpp OpenGLRenderer.h
    Renderer.cpp Renderer.h
    ShaderConfig.cpp ShaderConfig.h
    TerminalView.cpp TerminalView.h
    TextRenderer.cpp TextRenderer.h
)

option(LIBTERMINAL_VIEW_NATURAL_COORDS "Natural coordinates" ON)
if(LIBTERMINAL_VIEW_NATURAL_COORDS)
    target_compile_definitions(terminal_view PUBLIC LIBTERMINAL_VIEW_NATURAL_COORDS)
endif()

target_include_directories(terminal_view PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(terminal_view PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

set(TERMINAL_VIEW_LIBRARIES Qt5::Gui Freetype::Freetype terminal crispy::core crispy::gui)
if(APPLE)
    list(APPEND TERMINAL_VIEW_LIBRARIES PkgConfig::fontconfig)
    list(APPEND TERMINAL_VIEW_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux" OR APPLE)
    list(APPEND TERMINAL_VIEW_LIBRARIES fontconfig)
    list(APPEND TERMINAL_VIEW_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    list(APPEND TERMINAL_VIEW_LIBRARIES harfbuzz::harfbuzz)
    #list(APPEND GLTERM_LIBRARIES unofficial::fontconfig::fontconfig)
endif()

target_link_libraries(terminal_view PUBLIC ${TERMINAL_VIEW_LIBRARIES})
