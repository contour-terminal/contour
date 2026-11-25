# contour-extension-host: the process Contour runs extensions in, apart from itself. Its body is
# vtextension_host's runExtensionHost(); main.cpp only hands it the command line and the engine.
if(TARGET vtextension_wasmtime)
    add_executable(contour-extension-host main.cpp)
    target_link_libraries(contour-extension-host PRIVATE vtextension_host vtextension_wasmtime_helper_engine contour::wasmtime)
    # A console program, so that it opens no window; Contour starts it without a console (CREATE_NO_WINDOW).
    set_target_properties(contour-extension-host PROPERTIES WIN32_EXECUTABLE OFF)

    # Installed where contour finds it (vtextension_remote_process's locateExtensionHost()): beside contour.exe
    # on Windows; inside the app bundle on macOS, where the bundle's install carries it; elsewhere among the
    # private executables, CONTOUR_EXTENSION_HOST_INSTALL_DIR (../CMakeLists.txt), as no user runs it.
    if(WIN32)
        install(TARGETS contour-extension-host RUNTIME DESTINATION bin)
    elseif(CONTOUR_MACOS_APP_BUNDLE)
        add_custom_command(TARGET contour-extension-host POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:contour>/Helpers"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:contour-extension-host>"
                "$<TARGET_BUNDLE_CONTENT_DIR:contour>/Helpers/"
            VERBATIM
        )
    else()
        install(TARGETS contour-extension-host RUNTIME DESTINATION "${CONTOUR_EXTENSION_HOST_INSTALL_DIR}")
    endif()
else()
    message(STATUS "contour-extension-host is not built: it needs Wasmtime until its WAMR engine lands")
endif()
