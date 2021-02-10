add_library(text_shaper STATIC
    shaper.cpp shaper.h
    font.cpp font.h
    open_shaper.cpp open_shaper.h
    # TODO: directwrite_shaper.cpp directwrite_shaper.h
    # TODO: coretext_shaper.cpp coretext_shaper.h
)

find_package(Fontconfig REQUIRED)
find_package(Freetype REQUIRED)

set(TEXT_SHAPER_LIBS unicode::core)
list(APPEND TEXT_SHAPER_LIBS fmt::fmt-header-only)
list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
list(APPEND TEXT_SHAPER_LIBS Fontconfig::Fontconfig)

if(APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
    execute_process(
        COMMAND sh -c "brew --prefix harfbuzz | cut -d. -f1 | tr -d $'\n'"
        OUTPUT_VARIABLE HARFBUZZ_APPLE_INCLUDE)
    include_directories("${HARFBUZZ_APPLE_INCLUDE}/include")
    list(APPEND TEXT_SHAPER_LIBS  PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    # installed via vcpkg
    find_package(harfbuzz CONFIG REQUIRED)
    list(APPEND TEXT_SHAPER_LIBS harfbuzz::harfbuzz)
    list(APPEND TEXT_SHAPER_LIBS dwrite)
endif()

target_include_directories(text_shaper PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(text_shaper PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(text_shaper PRIVATE ${TEXT_SHAPER_LIBS})

message(STATUS "[text_shaper] Librarires: ${TEXT_SHAPER_LIBS}")
