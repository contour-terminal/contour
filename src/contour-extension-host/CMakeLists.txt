# contour-extension-host: the process Contour runs extensions in, apart from itself. Its body is
# vtextension_host's runExtensionHost(); main.cpp only hands it the command line and the engine.
if(TARGET vtextension_wasmtime)
    add_executable(contour-extension-host main.cpp)
    target_link_libraries(contour-extension-host PRIVATE vtextension_host vtextension_wasmtime_helper_engine contour::wasmtime)
    # A console program, so that it opens no window; Contour starts it without a console (CREATE_NO_WINDOW).
    set_target_properties(contour-extension-host PROPERTIES WIN32_EXECUTABLE OFF)
else()
    message(STATUS "contour-extension-host is not built: it needs Wasmtime until its WAMR engine lands")
endif()
