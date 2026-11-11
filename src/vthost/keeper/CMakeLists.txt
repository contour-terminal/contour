# The PTY keeper: the process that holds every shell's PTY, so the shells survive a crash of the daemon.
# It links core-cpp's foundation, the native protocol's byte level and nothing of the terminal engine --
# never vtbackend, vtparser, vtworkspace or vthost -- and what it must not include is checked by the
# `check_keeper_includes` lint gate.
add_library(vthost_keeper STATIC
    Codec.cpp
    Codec.hpp
    Frame.cpp
    Frame.hpp
    Keeper.cpp
    Keeper.hpp
    Messages.hpp
    Ports.hpp
    RestartPolicy.cpp
    RestartPolicy.hpp
    SessionTable.cpp
    SessionTable.hpp
    UniqueFd.hpp
)
target_include_directories(vthost_keeper PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
target_link_libraries(vthost_keeper PUBLIC
    vthost_image
    core::base
    core::codec
    core::platform
)

# The link list is part of the design: a keeper that links the engine could crash with it.
get_target_property(VTHOST_KEEPER_LINKS vthost_keeper LINK_LIBRARIES)
foreach(forbidden IN ITEMS vtbackend vtparser vtworkspace vthost)
    if(forbidden IN_LIST VTHOST_KEEPER_LINKS)
        message(FATAL_ERROR "vthost_keeper must not link ${forbidden}")
    endif()
endforeach()

option(VTHOST_KEEPER_TESTING "Enables building of unittests for the PTY keeper [default: ON]" ${CONTOUR_TESTING})
if(VTHOST_KEEPER_TESTING)
    enable_testing()
    add_executable(vthost_keeper_test
        test_main.cpp
        Codec_test.cpp
        Frame_test.cpp
        RestartPolicy_test.cpp
    )
    if(NOT WIN32)
        # The state machine's tests watch real pipes to see which descriptors the keeper closes.
        target_sources(vthost_keeper_test PRIVATE Keeper_test.cpp)
    endif()
    target_link_libraries(vthost_keeper_test vthost_keeper core::testing Catch2::Catch2 contour_test_trace)
    add_test(NAME vthost_keeper_test COMMAND $<TARGET_FILE:vthost_keeper_test>)
    set_tests_properties(vthost_keeper_test PROPERTIES LABELS "daemon;keeper" TIMEOUT 300)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vthost_keeper_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
