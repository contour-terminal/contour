# --------------------------------------------------------------------------------------------------------
# crispy::core

add_library(crispy-core INTERFACE)
add_library(crispy::core ALIAS crispy-core)
#target_include_directories(crispy-core PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_include_directories(crispy-core INTERFACE
    $<BUILD_INTERFACE:${${PROJECT_NAME}_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>)
target_compile_features(crispy-core INTERFACE cxx_std_17)
target_sources(crispy-core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/algorithm.h
    ${CMAKE_CURRENT_SOURCE_DIR}/compose.h
    ${CMAKE_CURRENT_SOURCE_DIR}/overloaded.h
    ${CMAKE_CURRENT_SOURCE_DIR}/reference.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stdfs.h
    ${CMAKE_CURRENT_SOURCE_DIR}/text.h
    ${CMAKE_CURRENT_SOURCE_DIR}/times.h
    ${CMAKE_CURRENT_SOURCE_DIR}/UTF8.h
)
target_link_libraries(crispy-core INTERFACE fmt::fmt-header-only)

# --------------------------------------------------------------------------------------------------------
# crispy::gui

find_package(Freetype REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Qt5 COMPONENTS Gui REQUIRED)  # apt install qtbase5-dev libqt5gui5

if(APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(fontconfig REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux")
    #find_package(fontconfig)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    #find_package(unofficial-fontconfig)
    find_package(harfbuzz CONFIG REQUIRED)
endif()

CIncludeMe(shaders/text.frag "${CMAKE_CURRENT_BINARY_DIR}/text_frag.h" "text_frag" "default_shaders")
CIncludeMe(shaders/text.vert "${CMAKE_CURRENT_BINARY_DIR}/text_vert.h" "text_vert" "default_shaders")

add_library(crispy-gui STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/text_frag.h"
    "${CMAKE_CURRENT_BINARY_DIR}/text_vert.h"
    FontManager.h FontManager.cpp
    Atlas.h
    AtlasRenderer.h AtlasRenderer.cpp
    TextShaper.h TextShaper.cpp
)
add_library(crispy::gui ALIAS crispy-gui)

option(LIBCRISPY_FONT_RENDER_CACHE "Enables internal render cache [default: ON]." ON)
if(LIBCRISPY_FONT_RENDER_CACHE)
    target_compile_definitions(crispy-gui PUBLIC LIBCRISPY_FONT_RENDER_CACHE=1)
endif()

target_include_directories(crispy-gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(crispy-gui PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)

set(LIBCRISPY_GUI_LIBRARIES Qt5::Gui Freetype::Freetype crispy::core)
if(APPLE)
    list(APPEND LIBCRISPY_GUI_LIBRARIES PkgConfig::fontconfig)
    list(APPEND LIBCRISPY_GUI_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux" OR APPLE)
    list(APPEND LIBCRISPY_GUI_LIBRARIES fontconfig)
    list(APPEND LIBCRISPY_GUI_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    list(APPEND LIBCRISPY_GUI_LIBRARIES harfbuzz::harfbuzz)
    #list(APPEND LIBCRISPY_GUI_LIBRARIES unofficial::fontconfig::fontconfig)
endif()
target_link_libraries(crispy-gui PUBLIC ${LIBCRISPY_GUI_LIBRARIES})

message(STATUS "[crispy::gui] Librarires: ${LIBCRISPY_GUI_LIBRARIES}")
message(STATUS "[crispy::gui] Font Render Cache: ${LIBCRISPY_FONT_RENDER_CACHE}")

# --------------------------------------------------------------------------------------------------------
# crispy_test

option(CRISPY_TESTING "Enables building of unittests for crispy library [default: ON]" ON)
if(CRISPY_TESTING)
    enable_testing()
    add_executable(crispy_test
        test_main.cpp
        UTF8_test.cpp
        compose_test.cpp
    )
    target_link_libraries(crispy_test fmt::fmt-header-only Catch2::Catch2 crispy::core)
    add_test(crispy_test ./crispy_test)
endif()
message(STATUS "[crispy] Compile unit tests: ${CRISPY_TESTING}")

