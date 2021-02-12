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
    ${CMAKE_CURRENT_SOURCE_DIR}/Atlas.h
)

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

