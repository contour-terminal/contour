set(text_shaper_SRC
    shaper.cpp shaper.h
    font.cpp font.h
    open_shaper.cpp open_shaper.h
)

if("${CMAKE_SYSTEM}" MATCHES "Windows")
    list(APPEND text_shaper_SRC directwrite_shaper.cpp directwrite_shaper.h)
endif()

# TODO: coretext_shaper.cpp coretext_shaper.h
add_library(text_shaper STATIC ${text_shaper_SRC})

set(TEXT_SHAPER_LIBS unicode::core)
list(APPEND TEXT_SHAPER_LIBS fmt::fmt-header-only)
list(APPEND TEXT_SHAPER_LIBS range-v3)

if(APPLE)
    find_package(PkgConfig REQUIRED)
    find_package(Fontconfig REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
    execute_process(
        COMMAND sh -c "brew --prefix harfbuzz | cut -d. -f1 | tr -d $'\n'"
        OUTPUT_VARIABLE HARFBUZZ_APPLE_INCLUDE)
    include_directories("${HARFBUZZ_APPLE_INCLUDE}/include")
    list(APPEND TEXT_SHAPER_LIBS Fontconfig::Fontconfig)
    list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux")
    find_package(Fontconfig REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
    list(APPEND TEXT_SHAPER_LIBS Fontconfig::Fontconfig)
    list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    find_package(harfbuzz CONFIG REQUIRED)
    find_package(Freetype REQUIRED)
    find_package(Fontconfig REQUIRED)
    list(APPEND TEXT_SHAPER_LIBS harfbuzz::harfbuzz)
    list(APPEND TEXT_SHAPER_LIBS Fontconfig::Fontconfig)
    list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
    list(APPEND TEXT_SHAPER_LIBS dwrite)
endif()

target_include_directories(text_shaper PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(text_shaper PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(text_shaper PRIVATE ${TEXT_SHAPER_LIBS})

message(STATUS "[text_shaper] Librarires: ${TEXT_SHAPER_LIBS}")
