# contour-extension-host: the process Contour runs extensions in, apart from itself. Its body is
# vtextension_host's runExtensionHost(); main.cpp only hands it the command line and the engine.
if(TARGET vtextension_wasmtime)
    add_executable(contour-extension-host main.cpp)
    target_link_libraries(contour-extension-host PRIVATE
        vtextension_host vtextension_host_sanitizer_options vtextension_wasmtime_helper_engine contour::wasmtime)
    # A console program, so that it opens no window; Contour starts it without a console (CREATE_NO_WINDOW).
    set_target_properties(contour-extension-host PROPERTIES WIN32_EXECUTABLE OFF)

    # Installed where contour finds it (vtextension_remote_process's locateExtensionHost()): beside contour.exe
    # on Windows; elsewhere among the private executables, CONTOUR_EXTENSION_HOST_INSTALL_DIR (../CMakeLists.txt),
    # as no user runs it. A macOS app bundle carries it instead: contour copies it in (../contour/CMakeLists.txt).
    if(WIN32)
        install(TARGETS contour-extension-host RUNTIME DESTINATION bin)
    elseif(NOT CONTOUR_MACOS_APP_BUNDLE)
        install(TARGETS contour-extension-host RUNTIME DESTINATION "${CONTOUR_EXTENSION_HOST_INSTALL_DIR}")
    endif()

    if(VTEXTENSION_TESTING)
        # The extension host misbehaving on purpose, one way per --hostile=<name> (testing/HostileModes.hpp): what
        # the crash and hostility tests start. No test mode ships in contour-extension-host itself. Never installed.
        add_executable(contour-extension-host-hostile testing/HostileHost.cpp testing/HostileModes.hpp)
        target_link_libraries(contour-extension-host-hostile PRIVATE
            vtextension_host vtextension_remote_wire vtextension_wasmtime_helper_engine contour::wasmtime)
        target_include_directories(contour-extension-host-hostile PRIVATE ${PROJECT_SOURCE_DIR}/src)
        set_target_properties(contour-extension-host-hostile PROPERTIES WIN32_EXECUTABLE OFF)
    endif()
else()
    message(STATUS "contour-extension-host is not built: it needs Wasmtime until its WAMR engine lands")
endif()
