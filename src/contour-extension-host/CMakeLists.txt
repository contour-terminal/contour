# contour-extension-host: the process Contour runs extensions in, apart from itself. Its body is
# vtextension_host's runExtensionHost(); main.cpp only hands it the command line and the engine, the one
# CONTOUR_WASM_RUNTIME names (../../CMakeLists.txt).
set(_extension_host_engine vtextension_${CONTOUR_WASM_RUNTIME}_helper_engine)

add_executable(contour-extension-host main.cpp)
target_link_libraries(contour-extension-host PRIVATE
    vtextension_host vtextension_host_sanitizer_options ${_extension_host_engine})
# A console program, so that it opens no window; Contour starts it without a console (CREATE_NO_WINDOW).
set_target_properties(contour-extension-host PROPERTIES WIN32_EXECUTABLE OFF)

# Installed where contour finds it (vtextension_remote_process's locateExtensionHost()): beside contour.exe
# on Windows; elsewhere among the private executables, CONTOUR_EXTENSION_HOST_INSTALL_DIR (../CMakeLists.txt),
# as no user runs it. A macOS app bundle carries it instead: contour copies it in (../contour/CMakeLists.txt).
# In contour's component, which is what the packages hold (CPACK_COMPONENTS_ALL): installed before contour's
# directory sets the default component, it would otherwise land in none they pack.
if(WIN32)
    install(TARGETS contour-extension-host RUNTIME DESTINATION bin COMPONENT contour)
elseif(NOT CONTOUR_MACOS_APP_BUNDLE)
    install(TARGETS contour-extension-host RUNTIME DESTINATION "${CONTOUR_EXTENSION_HOST_INSTALL_DIR}"
            COMPONENT contour)
endif()

if(VTEXTENSION_TESTING)
    # The extension host misbehaving on purpose, one way per --hostile=<name> (testing/HostileModes.hpp): what
    # the crash and hostility tests start. No test mode ships in contour-extension-host itself. Never installed.
    add_executable(contour-extension-host-hostile testing/HostileHost.cpp testing/HostileModes.hpp)
    target_link_libraries(contour-extension-host-hostile PRIVATE
        vtextension_host vtextension_remote_wire ${_extension_host_engine})
    target_include_directories(contour-extension-host-hostile PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(contour-extension-host-hostile PROPERTIES WIN32_EXECUTABLE OFF)

    if(NOT CONTOUR_WASM_RUNTIME STREQUAL "wamr" AND TARGET vtextension_wamr_helper_engine)
        # The extension host on WAMR's engine, beside the one this build runs: what a build without Wasmtime
        # ships, proven here by vtextension_wamr_helper_test. Never installed.
        add_executable(contour-extension-host-wamr main.cpp)
        target_link_libraries(contour-extension-host-wamr PRIVATE
            vtextension_host vtextension_host_sanitizer_options vtextension_wamr_helper_engine)
        set_target_properties(contour-extension-host-wamr PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
endif()
