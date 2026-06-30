add_library(vtmux STATIC
    Pane.cpp
    Pane.h
    Tab.cpp
    Tab.h
    SessionModel.cpp
    SessionModel.h
    ModelEvents.h
    Primitives.h
)
set_target_properties(vtmux PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
target_link_libraries(vtmux PUBLIC
    vtbackend
    crispy::core
)
target_include_directories(vtmux PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(VTMUX_TESTING "Enables building of unittests for vtmux [default: ON]" ${CONTOUR_TESTING})
if(VTMUX_TESTING)
    enable_testing()
    add_executable(vtmux_test
        test_main.cpp
        Pane_test.cpp
        Tab_test.cpp
        SessionModel_test.cpp
    )
    target_link_libraries(vtmux_test vtmux crispy::core Catch2::Catch2)
    add_test(NAME vtmux_test COMMAND $<TARGET_FILE:vtmux_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtmux_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
