find_package(PkgConfig REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(fontconfig REQUIRED IMPORTED_TARGET fontconfig)

set(HB_INSPECT_LIBRARIES fmt::fmt-header-only crispy::core Freetype::Freetype crispy::core)
if(APPLE)
    list(APPEND HB_INSPECT_LIBRARIES PkgConfig::fontconfig)
    list(APPEND HB_INSPECT_LIBRARIES PkgConfig::harfbuzz)
    execute_process(
        COMMAND sh -c "brew --prefix harfbuzz | cut -d. -f1 | tr -d $'\n'"
        OUTPUT_VARIABLE HARFBUZZ_APPLE_INCLUDE)
    include_directories(${HARFBUZZ_APPLE_INCLUDE}/include)
elseif("${CMAKE_SYSTEM}" MATCHES "Linux" OR APPLE)
    list(APPEND HB_INSPECT_LIBRARIES fontconfig)
    list(APPEND HB_INSPECT_LIBRARIES PkgConfig::harfbuzz)
elseif("${CMAKE_SYSTEM}" MATCHES "Windows")
    list(APPEND HB_INSPECT_LIBRARIES harfbuzz::harfbuzz)
    #list(APPEND HB_INSPECT_LIBRARIES unofficial::fontconfig::fontconfig)
endif()

add_executable(hb-inspect hb-inspect.cpp)
target_link_libraries(hb-inspect PRIVATE ${HB_INSPECT_LIBRARIES})
