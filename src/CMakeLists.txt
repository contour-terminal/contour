if(("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU") OR ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    #(TODO: first get rid of these warnings) add_compile_options(-Werror)
endif()

include(PedanticCompiler)

# Coverage instruments the first-party code under src/ only, never the fetched third-party code. Instrumented,
# a library's translation units built for a CPU extension -- libunicode compiles its scanner for AVX-512 -- reset
# their counters with that extension's instructions, and the runtime resets every unit's counters in each fork
# child (__gcov_fork): on a CPU without the extension every child died of SIGILL before it could exec.
if(CONTOUR_CODE_COVERAGE_ENABLED)
    add_compile_options(-g --coverage)
endif()

# Whether this build produces a macOS application bundle, as opposed to a plain executable.
#
# A .app exists to be double-clicked, and everything that assembles one -- the
# @executable_path/../Frameworks rpath, macdeployqt, the Info.plist keys, the icon, code signing,
# notarization and the DragNDrop disk image -- serves that window. A GUI-less build produces a
# command-line binary instead: none of that applies to it, and all of it needs a Qt that renders,
# which is the whole point of not building the frontend. So it installs like any other Unix CLI
# tool. Every bundle-only block (contour/, contour-extension-host/) is gated on this rather than on APPLE.
set(CONTOUR_MACOS_APP_BUNDLE OFF)
if(APPLE AND CONTOUR_FRONTEND_GUI)
    set(CONTOUR_MACOS_APP_BUNDLE ON)
endif()

# Before every module: their suites link its test support.
add_subdirectory(testing)
add_subdirectory(crispy)
add_subdirectory(text_shaper)
add_subdirectory(vtpty)
add_subdirectory(vtcontrol)
add_subdirectory(vtparser)
add_subdirectory(vtbackend)
if(CONTOUR_EXTENSIONS)
    # Where contour-extension-host is installed on Unix, a private executable of Contour's, and the way there
    # from the directory contour is installed in: how contour finds it (vtextension_remote_process).
    include(GNUInstallDirs)
    set(CONTOUR_EXTENSION_HOST_INSTALL_DIR "${CMAKE_INSTALL_LIBEXECDIR}/contour")
    # contour installs to CMAKE_INSTALL_BINDIR (../contour/CMakeLists.txt).
    file(RELATIVE_PATH CONTOUR_EXTENSION_HOST_RELATIVE_DIR
        "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBEXECDIR}/contour")
    add_subdirectory(vtextension)
    add_subdirectory(contour-extension-host)
endif()
add_subdirectory(vtconformance)
add_subdirectory(vtworkspace)
add_subdirectory(vthost)
add_subdirectory(vtrasterizer)

add_subdirectory(contour)
