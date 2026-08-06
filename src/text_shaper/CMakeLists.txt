set(text_shaper_SRC
    ClusterSpans.hpp
    Font.cpp Font.hpp
    FontLocator.hpp
    FontLocatorProvider.cpp FontLocatorProvider.hpp
    MockFontLocator.cpp MockFontLocator.hpp
    OpenShaper.cpp OpenShaper.hpp
    Shaper.cpp Shaper.hpp
)

if("${CMAKE_SYSTEM_NAME}" MATCHES "Windows")
    list(APPEND text_shaper_SRC DirectWriteAnalysisWrapper.hpp)
    list(APPEND text_shaper_SRC DirectWriteLocator.cpp DirectWriteLocator.hpp)
    list(APPEND text_shaper_SRC DirectWriteShaper.cpp DirectWriteShaper.hpp)
endif()
if(APPLE)
    list(APPEND text_shaper_SRC CoreTextLocator.hpp CoreTextLocator.mm)
endif()
if("${CMAKE_SYSTEM_NAME}" MATCHES "Linux" OR "${CMAKE_SYSTEM_NAME}" MATCHES "FreeBSD" OR "${CMAKE_SYSTEM_NAME}" MATCHES "OpenBSD")
    list(APPEND text_shaper_SRC FontconfigLocator.cpp FontconfigLocator.hpp)
endif()

# TODO: coretext_shaper.cpp coretext_shaper.hpp
add_library(text_shaper STATIC ${text_shaper_SRC})
set(TEXT_SHAPER_LIBS unicode::unicode boxed-cpp::boxed-cpp)
list(APPEND TEXT_SHAPER_LIBS Microsoft.GSL::GSL)

if(APPLE)
    enable_language(OBJCXX)
    enable_language(OBJC)
    list(APPEND TEXT_SHAPER_LIBS "-framework Foundation")
    list(APPEND TEXT_SHAPER_LIBS "-framework AppKit")
    list(APPEND TEXT_SHAPER_LIBS "-framework CoreText")
    find_package(PkgConfig REQUIRED)
    find_package(Fontconfig REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
    if(NOT DEFINED VCPKG_TOOLCHAIN)
        # Homebrew-only fallback. It must not run under vcpkg: `include_directories` is
        # global and would put Homebrew's harfbuzz headers ahead of the vcpkg ones we
        # actually link against, which is a header/library version mismatch waiting to
        # happen. Release packaging goes through vcpkg precisely to avoid Homebrew.
        execute_process(
            COMMAND sh -c "brew --prefix harfbuzz | cut -d. -f1 | tr -d $'\n'"
            OUTPUT_VARIABLE HARFBUZZ_APPLE_INCLUDE)
        include_directories("${HARFBUZZ_APPLE_INCLUDE}/include")
    endif()
    list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::harfbuzz)
    list(APPEND TEXT_SHAPER_LIBS Fontconfig::Fontconfig)
    pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo-ft)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::Cairo)
elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux" OR "${CMAKE_SYSTEM_NAME}" MATCHES "FreeBSD" OR "${CMAKE_SYSTEM_NAME}" MATCHES "OpenBSD")
    find_package(PkgConfig REQUIRED)
    find_package(Fontconfig REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo)
    list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::Cairo)
    if(CONTOUR_BUILD_STATIC)
        # qt exports harfbuzz as a library with such name
        list(APPEND TEXT_SHAPER_LIBS harfbuzz)
    else()
        pkg_check_modules(harfbuzz REQUIRED IMPORTED_TARGET harfbuzz)
        list(APPEND TEXT_SHAPER_LIBS PkgConfig::harfbuzz)
    endif()
    list(APPEND TEXT_SHAPER_LIBS Fontconfig::Fontconfig)
elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Windows")
    # installed via vcpkg
    find_package(PkgConfig REQUIRED)
    find_package(harfbuzz CONFIG REQUIRED)
    find_package(Freetype REQUIRED)
    # find_package(cairo CONFIG REQUIRED) -- unavailable in vcpkg for cairo
    pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo-ft)
    list(APPEND TEXT_SHAPER_LIBS PkgConfig::Cairo)
    list(APPEND TEXT_SHAPER_LIBS harfbuzz harfbuzz::harfbuzz)
    list(APPEND TEXT_SHAPER_LIBS Freetype::Freetype)
    list(APPEND TEXT_SHAPER_LIBS dwrite)
endif()

target_include_directories(text_shaper PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(text_shaper PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(text_shaper PRIVATE ${TEXT_SHAPER_LIBS})

message(STATUS "[text_shaper] Libraries: ${TEXT_SHAPER_LIBS}")

if(CONTOUR_TESTING)
    enable_testing()
    add_executable(text_shaper_test test_main.cpp ClusterSpans_test.cpp FontHash_test.cpp OpenShaper_test.cpp Scale_test.cpp)
    target_link_libraries(text_shaper_test PRIVATE text_shaper Catch2::Catch2 Microsoft.GSL::GSL unicode::unicode boxed-cpp::boxed-cpp crispy::core)
    add_test(NAME text_shaper_test COMMAND $<TARGET_FILE:text_shaper_test>)
    if(NOT WIN32)
        target_compile_options(text_shaper_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
