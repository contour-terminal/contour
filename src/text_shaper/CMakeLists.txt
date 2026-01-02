set(text_shaper_SRC
    font.cpp font.h
    font_locator.h
    font_locator_provider.cpp font_locator_provider.h
    mock_font_locator.cpp mock_font_locator.h
    open_shaper.cpp open_shaper.h
    shaper.cpp shaper.h
)

if("${CMAKE_SYSTEM_NAME}" MATCHES "Windows")
    list(APPEND text_shaper_SRC directwrite_analysis_wrapper.h)
    list(APPEND text_shaper_SRC directwrite_locator.cpp directwrite_locator.h)
    list(APPEND text_shaper_SRC directwrite_shaper.cpp directwrite_shaper.h)
endif()
if(APPLE)
    list(APPEND text_shaper_SRC coretext_locator.h coretext_locator.mm)
endif()
if("${CMAKE_SYSTEM_NAME}" MATCHES "Linux" OR "${CMAKE_SYSTEM_NAME}" MATCHES "FreeBSD" OR "${CMAKE_SYSTEM_NAME}" MATCHES "OpenBSD")
    list(APPEND text_shaper_SRC fontconfig_locator.cpp fontconfig_locator.h)
endif()

# TODO: coretext_shaper.cpp coretext_shaper.h
add_library(text_shaper STATIC ${text_shaper_SRC})
set_target_properties(text_shaper PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")

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
    execute_process(
        COMMAND sh -c "brew --prefix harfbuzz | cut -d. -f1 | tr -d $'\n'"
        OUTPUT_VARIABLE HARFBUZZ_APPLE_INCLUDE)
    include_directories("${HARFBUZZ_APPLE_INCLUDE}/include")
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
    add_executable(text_shaper_test test_main.cpp open_shaper_test.cpp)
    target_link_libraries(text_shaper_test PRIVATE text_shaper Catch2::Catch2 Microsoft.GSL::GSL unicode::unicode boxed-cpp::boxed-cpp crispy::core)
    add_test(NAME text_shaper_test COMMAND $<TARGET_FILE:text_shaper_test>)
    target_compile_options(text_shaper_test PRIVATE -Wno-c2y-extensions)
endif()
