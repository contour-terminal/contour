# SPDX-License-Identifier: Apache-2.0

# Test helpers that need no test framework, built wherever core-cpp is, the WebAssembly subset
# included: the Windows dialog suppression, a no-op everywhere else, the test doubles of
# core::base's interfaces (Environment.hpp), and the scoped fixtures a test that needs the real
# process state uses: a temporary directory, a working directory, an environment variable.
core_cpp_add_module(testing KIND STATIC
    HEADERS
        EnvHelper.hpp
        Environment.hpp
        ScopedTempDir.hpp
        ScopedWorkingDirectory.hpp
        SuppressWindowsDialogs.hpp
    SOURCES SuppressWindowsDialogs.cpp
    PUBLIC_LIBS core::base)

# An OBJECT library, because its one translation unit is referenced by nothing: in a static library
# the linker would drop it. Linking this adds its object to the executable itself.
core_cpp_add_module(testing_dialogs KIND OBJECT
    SOURCES SuppressWindowsDialogsAtStartup.cpp
    PUBLIC_LIBS core::testing)

# main() for every test binary: Catch2 with core-cpp's exit-code contract, after the LOG filter is
# applied to core::log. The only part of this module that needs Catch2, so the only part
# CORE_CPP_CATCH2_MAIN gates. No static-CRT twin: the lines below edit the target after it is
# declared, which a twin would not mirror -- its binaries would lack the dialog suppression and the
# skip exit code -- and Catch2 and core::testing_dialogs are built /MD whatever it links.
if(CORE_CPP_CATCH2_MAIN)
    core_cpp_add_module(testing_main KIND STATIC NO_STATIC_RUNTIME_TWIN
        HEADERS ExitCode.hpp
        SOURCES CatchMain.cpp ExitCode.cpp
        PUBLIC_LIBS core::testing core::log Catch2::Catch2)
    set_target_properties(core-cpp-testing_main PROPERTIES
        CORE_CPP_SKIP_EXIT_CODE ${CORE_CPP_SKIP_EXIT_CODE}
        EXPORT_PROPERTIES CORE_CPP_SKIP_EXIT_CODE)

    # The dialog suppression's object goes onto the link line of every executable that links
    # core::testing_main, as an interface link item. Inside this archive nothing would pull it in.
    # Spelled twice because an export copies $<TARGET_OBJECTS:...> without renaming the target in it:
    # an installed package names the imported core::testing_dialogs, which carries the installed
    # object (cmake/CoreCppInstall.cmake).
    target_link_libraries(core-cpp-testing_main INTERFACE
        core::testing_dialogs
        "$<BUILD_INTERFACE:$<TARGET_OBJECTS:core-cpp-testing_dialogs>>"
        "$<INSTALL_INTERFACE:$<TARGET_OBJECTS:core::testing_dialogs>>")
endif()

core_cpp_add_test(testing SOURCES
    CatchMain_test.cpp
    EnvHelper_test.cpp
    ExitCode_test.cpp
    ScopedTempDir_test.cpp
    ScopedWorkingDirectory_test.cpp
    SOURCES_WINDOWS
    windows/SuppressWindowsDialogs_test.cpp)
if(TEST core-cpp.testing)
    # CatchMain_test.cpp asserts what main() makes of it.
    set_tests_properties(core-cpp.testing PROPERTIES ENVIRONMENT "LOG=net")
endif()
