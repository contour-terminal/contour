#cmake_minimum_required(VERSION 3.10)
#project(vtparser VERSION "0.0.0" LANGUAGES CXX)

add_library(vtparser STATIC
    Parser.cpp
    Parser.h
    Parser-impl.h
    ParserEvents.h
)
set_target_properties(vtparser PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
target_link_libraries(vtparser PUBLIC
    Microsoft.GSL::GSL
    unicode::unicode
)
target_include_directories(vtparser PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(VTPARSER_TESTING "Enables building of unittests for vtparser [default: ON]" ${CONTOUR_TESTING})
if(VTPARSER_TESTING)
    enable_testing()
    add_executable(vtparser_test
        Parser_test.cpp
    )
    target_link_libraries(vtparser_test vtparser Catch2::Catch2WithMain)
    add_test(NAME vtparser_test COMMAND $<TARGET_FILE:vtparser_test>)
    target_compile_options(vtparser_test PRIVATE -Wno-c2y-extensions)
endif()
