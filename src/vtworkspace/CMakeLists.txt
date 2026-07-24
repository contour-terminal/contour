add_library(vtworkspace STATIC
    LayoutConvert.h
    LayoutTree.cpp
    LayoutTree.h
    Pane.cpp
    Pane.h
    PaneLayout.h
    Tab.cpp
    Tab.h
    SessionModel.cpp
    SessionModel.h
    ModelEvents.h
    Primitives.h
)
target_link_libraries(vtworkspace PUBLIC
    vtbackend
    crispy::core
)
target_include_directories(vtworkspace PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(VTWORKSPACE_TESTING "Enables building of unittests for vtworkspace [default: ON]" ${CONTOUR_TESTING})
if(VTWORKSPACE_TESTING)
    enable_testing()
    add_executable(vtworkspace_test
        test_main.cpp
        LayoutTree_test.cpp
        Pane_test.cpp
        PaneLayout_test.cpp
        Tab_test.cpp
        SessionModel_test.cpp
    )
    target_link_libraries(vtworkspace_test vtworkspace crispy::core Catch2::Catch2)
    add_test(NAME vtworkspace_test COMMAND $<TARGET_FILE:vtworkspace_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtworkspace_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
