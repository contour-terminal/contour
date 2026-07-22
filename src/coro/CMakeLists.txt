add_library(coro INTERFACE
    Awaitable.hpp
    Cancellation.hpp
    Task.hpp
    UniqueCoroHandle.hpp
    WhenAll.hpp
    WhenAny.hpp
)
target_include_directories(coro INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
target_compile_features(coro INTERFACE cxx_std_23)

option(CORO_TESTING "Enables building of unittests for coro [default: ON]" ${CONTOUR_TESTING})
if(CORO_TESTING)
    enable_testing()
    add_executable(coro_test
        test_main.cpp
        Task_test.cpp
        WhenAll_test.cpp
        WhenAny_test.cpp
    )
    target_link_libraries(coro_test coro crispy::core Catch2::Catch2)
    add_test(NAME coro_test COMMAND $<TARGET_FILE:coro_test>)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(coro_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
