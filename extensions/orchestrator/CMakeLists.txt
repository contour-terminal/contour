# SPDX-License-Identifier: Apache-2.0
#
# The agent orchestrator: a project of its own, built inside Contour today and extracted into its own
# repository later. It therefore includes nothing from Contour's src/; its contract with Contour is the
# daemon extension world, the manifest and the `contour agent` verbs.
cmake_minimum_required(VERSION 3.25 FATAL_ERROR)
project(orchestrator LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Contour's gates, mirroring src/CMakeLists.txt: both are directory-scoped there, so a project beside src/
# has to ask for them itself. Standalone, PedanticCompiler is absent and the include does nothing.
include(PedanticCompiler OPTIONAL)
if(CONTOUR_CODE_COVERAGE_ENABLED)
    add_compile_options(-g --coverage)
endif()

option(ORCHESTRATOR_TESTING "Enables building of unittests for the agent orchestrator [default: CONTOUR_TESTING, OFF standalone]" ${CONTOUR_TESTING})

# Inside Contour (whose CONTOUR_EXTENSIONS adds this directory), nothing of the native build ships: its libraries
# exist for the tests and, through the manifest tool, for the guest. A build that wants neither builds none of it.
# Standalone, and in the guest's own sub-build, CONTOUR_EXTENSIONS is not set and everything is built.
if(CONTOUR_EXTENSIONS AND NOT ORCHESTRATOR_TESTING AND NOT CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST)
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# The sans-IO core.
add_library(orchestrator_core STATIC
    core/ArgumentKinds.hpp
    core/Commands.cpp
    core/Commands.hpp
    core/Config.cpp
    core/Config.hpp
    core/ConfigJson.hpp
    core/Engine.cpp
    core/Engine.hpp
    core/EngineDetail.hpp
    core/EngineDispatch.cpp
    core/EnginePoll.cpp
    core/Finished.cpp
    core/Finished.hpp
    core/GitHub.cpp
    core/GitHub.hpp
    core/GitHubJson.hpp
    core/Hooks.cpp
    core/Hooks.hpp
    core/HooksJson.hpp
    core/Json.hpp
    core/JsonPrefix.cpp
    core/JsonPrefix.hpp
    core/Model.cpp
    core/Model.hpp
    core/Naming.cpp
    core/Naming.hpp
    core/OperationState.cpp
    core/OperationState.hpp
    core/Overloaded.hpp
    core/Prompt.cpp
    core/Prompt.hpp
    core/Provider.cpp
    core/Provider.hpp
    core/Run.cpp
    core/Run.hpp
    core/Reconcile.cpp
    core/Reconcile.hpp
    core/RunJson.hpp
    core/Sanitize.cpp
    core/Sanitize.hpp
    core/Utf8.hpp
)
target_compile_features(orchestrator_core PUBLIC cxx_std_23)
# PRIVATE: glaze stays in the .cpp files, and no header of the core may pull it in (scripts/include-budget.txt).
target_link_libraries(orchestrator_core PRIVATE glaze::glaze)
# The include root is the directory above this project, so that sources spell <orchestrator/core/X.hpp>
# (<core/...> is core-cpp's). An extracted checkout must therefore live in a directory named `orchestrator`.
target_include_directories(orchestrator_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

# The dashboard: the engine's lists drawn with core-cpp's headless terminal UI, laid out by breakpoint. Guest code
# like the core: the guest serves it in its tab, and the native build tests it against golden screens.
add_library(orchestrator_view STATIC
    view/Band.hpp
    view/Dashboard.cpp
    view/Dashboard.hpp
    view/Keys.cpp
    view/Keys.hpp
    view/Sections.cpp
    view/Sections.hpp
    view/Theme.cpp
    view/Theme.hpp
)
target_link_libraries(orchestrator_view PUBLIC orchestrator_core core::tui_core)

# The guest's driver: the engine driven through the daemon world's host calls (guest/HostCalls.hpp). Native too,
# so that its tests run against a fake host; the WebAssembly module adds the adapter to the generated bindings.
add_library(orchestrator_guest_driver STATIC
    guest/Driver.cpp
    guest/Driver.hpp
    guest/HostCalls.hpp
)
target_link_libraries(orchestrator_guest_driver PUBLIC orchestrator_core)

# Prints the manifest the declared tables make: the module's custom section is generated from it, and a test
# compares it with the corpus. A host tool; never installed.
add_executable(orchestrator_manifest EXCLUDE_FROM_ALL tools/ManifestMain.cpp)
target_link_libraries(orchestrator_manifest PRIVATE orchestrator_core)

# The prototype driver's pure half: the pane listing, the order of its work, the store's file names and the
# environment its children get. Portable, so the tests cover it everywhere.
add_library(orchestrator_proto_logic STATIC
    proto/Environment.cpp
    proto/Environment.hpp
    proto/Panes.cpp
    proto/Panes.hpp
    proto/PanesJson.hpp
    proto/RunChanges.cpp
    proto/RunChanges.hpp
    proto/RunRefs.cpp
    proto/RunRefs.hpp
    proto/StepQueue.cpp
    proto/StepQueue.hpp
    proto/StoreKeys.cpp
    proto/StoreKeys.hpp
)
target_link_libraries(orchestrator_proto_logic PUBLIC orchestrator_core PRIVATE glaze::glaze)

# The prototype driver: the engine against git, gh and `contour api`, so the workflow can be tried before the
# extension host exists. POSIX only (posix_spawn, poll), outside the default build, and never installed.
if(UNIX)
    add_executable(orchestrator_proto EXCLUDE_FROM_ALL
        proto/Control.cpp
        proto/Control.hpp
        proto/Driver.cpp
        proto/Driver.hpp
        proto/Host.cpp
        proto/Host.hpp
        proto/HostJson.hpp
        proto/Main.cpp
        proto/Prompt.cpp
        proto/Prompt.hpp
        proto/Signals.cpp
        proto/Signals.hpp
        proto/Spawn.cpp
        proto/Spawn.hpp
        proto/Store.cpp
        proto/Store.hpp
    )
    target_link_libraries(orchestrator_proto PRIVATE orchestrator_proto_logic orchestrator_core glaze::glaze)
endif()

if(ORCHESTRATOR_TESTING)
    enable_testing()
    add_executable(orchestrator_test
        core/test_main.cpp
        core/ArgumentKinds_test.cpp
        core/Commands_test.cpp
        core/Config_test.cpp
        core/Engine_test.cpp
        core/Fixtures.hpp
        core/Finished_test.cpp
        core/GitHub_test.cpp
        core/JsonPrefix_test.cpp
        core/Naming_test.cpp
        core/OperationState_test.cpp
        core/Prompt_test.cpp
        core/Provider_test.cpp
        core/Reconcile_test.cpp
        core/Run_test.cpp
        core/Sanitize_test.cpp
        core/Utf8_test.cpp
        guest/Driver_test.cpp
        proto/Environment_test.cpp
        proto/Panes_test.cpp
        proto/RunChanges_test.cpp
        proto/RunRefs_test.cpp
        proto/StepQueue_test.cpp
        proto/StoreKeys_test.cpp
        view/Dashboard_test.cpp
        view/Keys_test.cpp
        view/Sections_test.cpp
        view/ViewFixtures.cpp
        view/ViewFixtures.hpp
    )
    # glaze: GitHub_test reads the projected fixtures generically, to check them against the declared filter.
    target_link_libraries(orchestrator_test PRIVATE orchestrator_core orchestrator_guest_driver orchestrator_proto_logic orchestrator_view core::testing Catch2::Catch2 glaze::glaze)
    # Fixtures are read from the source tree: the manifest corpus Contour's host runs too, and the recorded
    # output of the tools the extension runs.
    target_compile_definitions(orchestrator_test PRIVATE
        ORCHESTRATOR_CONTRACT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contract"
        ORCHESTRATOR_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/testdata"
        ORCHESTRATOR_VIEW_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/view/testdata"
    )
    add_test(NAME orchestrator_test COMMAND orchestrator_test)
    set_tests_properties(orchestrator_test PROPERTIES LABELS daemon)

    # The manifest the module carries is the one the corpus holds, byte for byte: the tool's own output, not
    # just manifestText(), so that a line end the tool changes is caught too.
    add_dependencies(orchestrator_test orchestrator_manifest)
    add_test(NAME orchestrator_manifest_matches_corpus
             COMMAND ${CMAKE_COMMAND}
                     -DMANIFEST_TOOL=$<TARGET_FILE:orchestrator_manifest>
                     -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/contract/manifest-v2/valid/orchestrator.manifest
                     -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/orchestrator_manifest.out
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ManifestMatchesCorpus.cmake)
    set_tests_properties(orchestrator_manifest_matches_corpus PROPERTIES LABELS daemon)

    if(NOT WIN32)
        # Catch2's TEST_CASE expands __COUNTER__, which Clang flags as a C2y extension under -Werror.
        target_compile_options(orchestrator_test PRIVATE -Wno-c2y-extensions)
    endif()
endif()

# The WebAssembly module: the guest/ project, built with the WASI C++ toolchain (cmake/ContourWasiCxx.cmake) in a
# sub-build of its own, carrying the manifest the host tool generates as its custom section.
if(CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST)
    set(_orchestrator_manifest_section "${CMAKE_CURRENT_BINARY_DIR}/guest/ManifestSection.cpp")
    add_custom_command(
        OUTPUT "${_orchestrator_manifest_section}"
        COMMAND ${CMAKE_COMMAND}
                -DMANIFEST_TOOL=$<TARGET_FILE:orchestrator_manifest>
                -DOUTPUT=${_orchestrator_manifest_section}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedManifest.cmake
        DEPENDS orchestrator_manifest ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedManifest.cmake
        COMMENT "Generating the orchestrator guest's manifest section"
        VERBATIM)
    add_custom_target(orchestrator_manifest_section DEPENDS "${_orchestrator_manifest_section}")
    contour_add_wasm_cxx_guest(NAME orchestrator
                               SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/guest
                               OUTPUT_VAR ORCHESTRATOR_WASM_MODULE
                               CMAKE_ARGS "-DORCHESTRATOR_MANIFEST_SECTION_SOURCE=${_orchestrator_manifest_section}")
    add_dependencies(orchestrator_wasm_cxx orchestrator_manifest_section)
endif()
