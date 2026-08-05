# The headless application: every `contour <verb>` that does not open a window -- capture, cat,
# generate, daemon, the documentation writers.
#
# Qt-free, and this is the target a CONTOUR_FRONTEND_GUI=OFF build is built around: what is left
# of the binary without a frontend is exactly contour_cli plus the configuration model beneath it.

# {{{ shell-integration scripts, embedded
# One row per supported shell. Adding a shell is dropping its script beside the others in
# shell-integration/ and adding it here: the embedded data, the `generate integration` dispatch
# and that verb's help text all derive from this list.
#
# Set in the parent scope as well, because the parent's e2e test registration asserts that
# `contour generate integration` emits exactly these shells' scripts.
set(CONTOUR_SHELL_INTEGRATION_SHELLS bash fish tcsh zsh)
set(CONTOUR_SHELL_INTEGRATION_SHELLS "${CONTOUR_SHELL_INTEGRATION_SHELLS}" PARENT_SCOPE)

list(TRANSFORM CONTOUR_SHELL_INTEGRATION_SHELLS
     PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/shell-integration/shell-integration."
     OUTPUT_VARIABLE _shell_integration_scripts)

# The scripts used to reach the binary through resources.qrc, which made `contour generate
# integration` -- a verb a GUI-less build keeps -- depend on Qt's resource system. See
# EmbedShellIntegration.cmake for why the replacement is a CMake script.
set(_shell_integration_data "${CMAKE_CURRENT_BINARY_DIR}/ShellIntegrationData.h")
add_custom_command(
    OUTPUT "${_shell_integration_data}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSHELLS=${CONTOUR_SHELL_INTEGRATION_SHELLS}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUTPUT=${_shell_integration_data}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShellIntegration.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/EmbedShellIntegration.cmake" ${_shell_integration_scripts}
    COMMENT "Embedding shell-integration scripts"
    VERBATIM
)
# }}}

# The generated header is listed as a source so that building this target runs the custom command
# that produces it; nothing compiles it on its own.
add_library(contour_cli STATIC
    CaptureScreen.cpp CaptureScreen.h
    CatImageArgs.h
    ContourApp.cpp ContourApp.h
    ShellIntegration.cpp ShellIntegration.h
    "${_shell_integration_data}"
)

target_link_libraries(contour_cli
    PUBLIC contour_config
    PRIVATE vthost
)

# The binary dir is PUBLIC so that ShellIntegration.cpp's <ShellIntegrationData.h> resolves, and
# so it keeps resolving for any consumer that compiles this target's sources.
target_include_directories(contour_cli PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
