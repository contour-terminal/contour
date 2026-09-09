# Tracy frame profiler integration.
#
# Three independent options, all OFF by default. With all three off nothing is fetched and no
# compiled output changes: contour::tracy merely puts the first-party no-op header on the include
# path, so call sites can use Tracy's macros unconditionally.
#
# One version pin feeds all three. The client compiled into contour and the tools that read its
# captures must be the same version -- Tracy's capture protocol is version-specific.
set(TRACY_VERSION "0.14.1" CACHE STRING "Tracy version used for the client and the tools")

# Tracy's own options are spelled as negatives; ours is spelled as the thing you want, so the
# CMake line reads as the intent rather than as a double negative.
if(CONTOUR_TRACY_SAMPLING)
    set(CONTOUR_TRACY_NO_SAMPLING OFF)
else()
    set(CONTOUR_TRACY_NO_SAMPLING ON)
endif()

add_library(contour_tracy INTERFACE)
add_library(contour::tracy ALIAS contour_tracy)

if(CONTOUR_TRACY)
    # Acquisition mirrors the explicit GSL block in ContourThirdParties.cmake rather than using
    # HandleThirdparty(): that macro tests `if(TARGET ${_TARGET})` and Tracy's imported target is
    # Tracy::TracyClient, not Tracy, so the embedded and system checks would never match.
    if(NOT TARGET Tracy::TracyClient)
        find_package(Tracy ${TRACY_VERSION} QUIET)
        if(Tracy_FOUND)
            set(THIRDPARTY_BUILTIN_Tracy "system package" CACHE INTERNAL "")
        elseif(CONTOUR_USE_CPM)
            message(STATUS "Using CPM to fetch Tracy")
            CPMAddPackage(
                NAME tracy
                GITHUB_REPOSITORY wolfpld/tracy
                GIT_TAG v${TRACY_VERSION}
                EXCLUDE_FROM_ALL YES
                OPTIONS
                    # Upstream defaults this to OFF, which compiles the client away to nothing and
                    # leaves a profiling build silently producing no data at all.
                    "TRACY_ENABLE ON"
                    "TRACY_STATIC ON"
                    # Confines both the listen socket and the client's announcement broadcast to
                    # loopback (Tracy broadcasts to 127.255.255.255 under this option), so a
                    # profiling build is discoverable by a profiler on this machine and invisible to
                    # the local network.
                    #
                    # NOT TRACY_NO_BROADCAST. That was set here first, for the same privacy reason,
                    # and it is the wrong tool: it compiles out the announcement altogether, so the
                    # profiler GUI's client list stays empty and the only way to attach is to type
                    # 127.0.0.1 by hand. TRACY_ONLY_LOCALHOST already covers the concern.
                    "TRACY_ONLY_LOCALHOST ON"
                    # Sampling is what fills the parts of the timeline we have not instrumented, so
                    # it is on by default -- it is how you find the next place a zone belongs. It is
                    # also what puts a row on the timeline for EVERY OS thread, including the GL
                    # driver's (Mesa names them <process>:cs0, :gdrv0, :gl0, so they look like
                    # ours). Those rows can never say anything: we cannot instrument the driver, and
                    # its stacks do not symbolize because the system libraries are stripped.
                    # -DCONTOUR_TRACY_SAMPLING=OFF drops them, and shrinks the trace considerably.
                    "TRACY_NO_SAMPLING ${CONTOUR_TRACY_NO_SAMPLING}"
                    "TRACY_NO_CONTEXT_SWITCH ${CONTOUR_TRACY_NO_SAMPLING}"
            )
            set(THIRDPARTY_BUILTIN_Tracy "embedded (CPM)" CACHE INTERNAL "")
        else()
            message(FATAL_ERROR "Could not find Tracy ${TRACY_VERSION}. "
                "Install it, or set CONTOUR_USE_CPM=ON.")
        endif()
    else()
        set(THIRDPARTY_BUILTIN_Tracy "embedded" CACHE INTERNAL "")
    endif()

    # TRACY_ENABLE is a PUBLIC compile definition of TracyClient, so linking is all a consumer needs.
    target_link_libraries(contour_tracy INTERFACE Tracy::TracyClient)

    # Vendored code is not ours to lint, and Tracy ships no .clang-tidy -- tidy aborts the build
    # with "no checks enabled" otherwise. Setting the per-target property is order-independent,
    # unlike unsetting CMAKE_CXX_CLANG_TIDY around the fetch.
    if(TARGET TracyClient)
        set_target_properties(TracyClient PROPERTIES CXX_CLANG_TIDY "")
    endif()

    message(STATUS "Tracy: instrumentation ENABLED (v${TRACY_VERSION}), "
                   "call-stack sampling ${CONTOUR_TRACY_SAMPLING}")
else()
    set(THIRDPARTY_BUILTIN_Tracy "(disabled)" CACHE INTERNAL "")
    # SYSTEM so the stub can never contribute a warning to a -Werror build. BUILD_INTERFACE only:
    # the stub is a build-time shim and is not part of anything we install.
    target_include_directories(contour_tracy SYSTEM INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/crispy/tracy-stub>)
    message(STATUS "Tracy: instrumentation disabled (using no-op stub)")
endif()

# {{{ Tracy CLI tools and profiler GUI
#
# tracy-capture records a running client to a .tracy file; tracy-csvexport turns that file into
# per-zone statistics on stdout, which is the terminal-readable way to compare two commits. The
# profiler GUI is the interactive view of the same trace. None of the three is packaged on Arch,
# which is why they are buildable from this tree at all.
#
# ExternalProject, not add_subdirectory: each of Tracy's tool directories is a CMake project in its
# own right that CPM-fetches its own dependencies -- capstone, zstd, PPQSort and json for the CLI
# tools, and glfw, freetype, imgui, nativefiledialog, md4c, base64, tidy, usearch, pugixml and
# libcurl on top of those for the GUI. Adding them as subdirectories would put a dozen third-party
# projects into our build graph, collide with our own Freetype, and merge their install(TARGETS)
# rules into our package. Isolated binary directories keep all of that out.
if(CONTOUR_TRACY_TOOLS OR CONTOUR_TRACY_GUI)
    include(ExternalProject)

    set(CONTOUR_TRACY_TOOLS_PREFIX "${CMAKE_BINARY_DIR}/tracy-tools")

    # One source download shared by every tool, so the GUI and the CLI tools cannot end up built
    # from different Tracy revisions -- their capture format would not match.
    ExternalProject_Add(tracy_source
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v${TRACY_VERSION}
        GIT_SHALLOW ON
        PREFIX "${CONTOUR_TRACY_TOOLS_PREFIX}"
        # Not ${PREFIX}/src: ExternalProject keeps its own stamp directory at
        # ${PREFIX}/src/<name>-stamp, and cloning over that path destroys it -- the clone succeeds
        # and the build then fails on "Failed to copy script-last-run stamp file".
        SOURCE_DIR "${CONTOUR_TRACY_TOOLS_PREFIX}/tracy-src"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        EXCLUDE_FROM_ALL ON
    )

    # Adds one of Tracy's tool projects as an isolated ExternalProject.
    #   name -- the tool's directory under the Tracy source tree (e.g. "capture")
    #   ARGN -- extra -D arguments passed to that project's own CMake configure step
    function(contour_add_tracy_tool name)
        ExternalProject_Add(tracy_${name}
            DEPENDS tracy_source
            SOURCE_DIR "${CONTOUR_TRACY_TOOLS_PREFIX}/tracy-src/${name}"
            BINARY_DIR "${CONTOUR_TRACY_TOOLS_PREFIX}/build-${name}"
            DOWNLOAD_COMMAND ""
            CMAKE_ARGS
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_INSTALL_PREFIX=${CONTOUR_TRACY_TOOLS_PREFIX}
                # Tracy's tools are not ours to lint, nor to hold to our warning settings.
                -DCMAKE_CXX_CLANG_TIDY=
                ${ARGN}
            INSTALL_COMMAND ${CMAKE_COMMAND} --install . --prefix ${CONTOUR_TRACY_TOOLS_PREFIX}
            EXCLUDE_FROM_ALL ON
        )
    endfunction()
endif()

if(CONTOUR_TRACY_TOOLS)
    contour_add_tracy_tool(capture)
    contour_add_tracy_tool(csvexport)

    add_custom_target(tracy-tools DEPENDS tracy_capture tracy_csvexport)
    message(STATUS "Tracy: building capture + csvexport into ${CONTOUR_TRACY_TOOLS_PREFIX}/bin")
endif()

if(CONTOUR_TRACY_GUI)
    # LEGACY selects X11 over Wayland in Tracy's own build; DOWNLOAD_GLFW because glfw is not
    # installed here and Tracy's pkg-config probe would otherwise fail the configure outright.
    contour_add_tracy_tool(profiler -DLEGACY=${CONTOUR_TRACY_GUI_X11} -DDOWNLOAD_GLFW=ON)

    add_custom_target(tracy-gui DEPENDS tracy_profiler)
    message(STATUS "Tracy: building the profiler GUI into ${CONTOUR_TRACY_TOOLS_PREFIX}/bin")
endif()
# }}}
