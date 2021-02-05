include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/FilesystemResolver.cmake")

# --------------------------------------------------------------------------------------------------------
# crispy::core

add_library(crispy-core INTERFACE)
add_library(crispy::core ALIAS crispy-core)

set(CRISPY_CORE_LIBS fmt::fmt-header-only unicode::core)
if(${USING_BOOST_FILESYSTEM})
    target_compile_definitions(crispy-core INTERFACE USING_BOOST_FILESYSTEM=1)
    list(APPEND CRISPY_CORE_LIBS Boost::filesystem)
endif()

target_link_libraries(crispy-core INTERFACE ${CRISPY_CORE_LIBS})
target_compile_features(crispy-core INTERFACE cxx_std_17)
target_include_directories(crispy-core INTERFACE
    $<BUILD_INTERFACE:${${PROJECT_NAME}_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
target_sources(crispy-core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Comparison.h
    ${CMAKE_CURRENT_SOURCE_DIR}/algorithm.h
    ${CMAKE_CURRENT_SOURCE_DIR}/base64.h
    ${CMAKE_CURRENT_SOURCE_DIR}/compose.h
    ${CMAKE_CURRENT_SOURCE_DIR}/escape.h
    ${CMAKE_CURRENT_SOURCE_DIR}/indexed.h
    ${CMAKE_CURRENT_SOURCE_DIR}/logger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/overloaded.h
    ${CMAKE_CURRENT_SOURCE_DIR}/reference.h
    ${CMAKE_CURRENT_SOURCE_DIR}/span.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stdfs.h
    ${CMAKE_CURRENT_SOURCE_DIR}/times.h
)

# --------------------------------------------------------------------------------------------------------
# crispy::gui

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
    find_package(Fontconfig REQUIRED)
    find_package(harfbuzz CONFIG REQUIRED)
endif()

add_library(crispy-gui STATIC
    Atlas.h
    text/Font.h text/Font.cpp
    text/FontLoader.h text/FontLoader.cpp
    text/TextShaper.h text/TextShaper.cpp
)
add_library(crispy::gui ALIAS crispy-gui)

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
    list(APPEND LIBCRISPY_GUI_LIBRARIES Fontconfig::Fontconfig)
    list(APPEND LIBCRISPY_GUI_LIBRARIES harfbuzz::harfbuzz)
    #list(APPEND LIBCRISPY_GUI_LIBRARIES unofficial::fontconfig::fontconfig)
endif()
target_link_libraries(crispy-gui PUBLIC ${LIBCRISPY_GUI_LIBRARIES})

message(STATUS "[crispy::gui] Librarires: ${LIBCRISPY_GUI_LIBRARIES}")

# --------------------------------------------------------------------------------------------------------
# crispy_test

option(CRISPY_TESTING "Enables building of unittests for crispy library [default: ON]" ON)
if(CRISPY_TESTING)
    enable_testing()
    add_executable(crispy_test
        base64_test.cpp
        indexed_test.cpp
        compose_test.cpp
        utils_test.cpp
        sort_test.cpp
        test_main.cpp
    )
    target_link_libraries(crispy_test fmt::fmt-header-only Catch2::Catch2 crispy::core)
    add_test(crispy_test ./crispy_test)
endif()
message(STATUS "[crispy] Compile unit tests: ${CRISPY_TESTING}")

