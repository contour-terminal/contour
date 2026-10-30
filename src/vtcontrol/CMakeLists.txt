add_library(vtcontrol STATIC
    Api.hpp
    Catalog.cpp
    Catalog.hpp
    ControlApi.cpp
    ControlApi.hpp
    ControlError.hpp
    Describe.cpp
    Describe.hpp
    Dispatch.cpp
    Dispatch.hpp
    EnumTable.hpp
    Events.cpp
    Events.hpp
    Json.cpp
    Json.hpp
    ObjectRef.cpp
    ObjectRef.hpp
    Reflect.hpp
    Target.cpp
    Target.hpp
    Uuid.cpp
    Uuid.hpp
    Wit.cpp
    Wit.hpp
    testing/FakeControlApi.hpp
    testing/JsonProbe.hpp
    testing/SequentialUuidSource.hpp
)
target_link_libraries(vtcontrol PUBLIC
    core::base
)
# Private, so that it stays out of every header but Json.hpp: a target reading or writing the API's JSON
# includes Json.hpp from its .cpp files and links glaze itself, and one that only names the vocabulary
# (the GUI, through vthost's headers) never sees it -- a header that pulled glaze in would not compile there.
target_link_libraries(vtcontrol PRIVATE
    core::platform
    glaze::glaze
)
target_include_directories(vtcontrol PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(VTCONTROL_TESTING "Enables building of unittests for vtcontrol [default: ON]" ${CONTOUR_TESTING})
if(VTCONTROL_TESTING)
    enable_testing()
    add_executable(vtcontrol_test
        test_main.cpp
        Api_test.cpp
        Catalog_test.cpp
        Describe_test.cpp
        Dispatch_test.cpp
        Grants_test.cpp
        Events_test.cpp
        Json_test.cpp
        ObjectRef_test.cpp
        Target_test.cpp
        Uuid_test.cpp
        Wit_test.cpp
    )
    target_link_libraries(vtcontrol_test vtcontrol glaze::glaze core::platform core::testing Catch2::Catch2 contour_test_trace)
    # The control WIT package is generated from the catalog; the golden test reads it from the source tree,
    # and rewrites it when UPDATE_WIT=1 is set.
    target_compile_definitions(vtcontrol_test PRIVATE VTCONTROL_WIT_DIR="${PROJECT_SOURCE_DIR}/wit/control")
    add_test(NAME vtcontrol_test COMMAND $<TARGET_FILE:vtcontrol_test>)
    set_tests_properties(vtcontrol_test PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(vtcontrol_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()
