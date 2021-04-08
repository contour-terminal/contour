include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/FilesystemResolver.cmake")

# --------------------------------------------------------------------------------------------------------
# crispy::core

set(crispy_SOURCES
    App.cpp App.h
    CLI.cpp CLI.h
    Comparison.h
    algorithm.h
    base64.h
    compose.h
    debuglog.h
    escape.h
    indexed.h
    overloaded.h
    reference.h
    span.h
    stdfs.h
    times.h
)

add_library(crispy-core ${crispy_SOURCES})
add_library(crispy::core ALIAS crispy-core)

set(CRISPY_CORE_LIBS fmt::fmt-header-only unicode::core)
if(${USING_BOOST_FILESYSTEM})
    target_compile_definitions(crispy-core PUBLIC USING_BOOST_FILESYSTEM=1)
    list(APPEND CRISPY_CORE_LIBS Boost::filesystem)
else()
    list(APPEND CRISPY_CORE_LIBS ${FILESYSTEM_LIBS})
endif()

target_link_libraries(crispy-core PUBLIC ${CRISPY_CORE_LIBS})
target_compile_features(crispy-core PUBLIC cxx_std_17)
target_include_directories(crispy-core PUBLIC
    $<BUILD_INTERFACE:${${PROJECT_NAME}_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

# --------------------------------------------------------------------------------------------------------
# crispy_test

option(CRISPY_TESTING "Enables building of unittests for crispy library [default: ON]" ON)
if(CRISPY_TESTING)
    enable_testing()
    add_executable(crispy_test
        CLI_test.cpp
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

