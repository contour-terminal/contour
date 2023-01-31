#cmake_minimum_required(VERSION 3.10)
#project(vtparser VERSION "0.0.0" LANGUAGES CXX)

add_library(vtparser STATIC
    Parser.cpp
    Parser.h
    Parser-impl.h
    ParserEvents.h
)
set_target_properties(vtparser PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
target_link_libraries(vtparser PUBLIC crispy::core fmt::fmt-header-only Microsoft.GSL::GSL)

option(VTPARSER_TESTING "Enables building of unittests for vtparser [default: ON]" ON)
if(VTPARSER_TESTING)
    enable_testing()
    add_executable(vtparser_test
        test_main.cpp
        Parser_test.cpp
    )
    target_link_libraries(vtparser_test vtparser Catch2::Catch2)
    add_test(vtparser_test ./vtparser_test)
endif()
