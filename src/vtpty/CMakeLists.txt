if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(LINUX TRUE)
endif()

# Platform suffix can be either _win32 or _unix.
if(WIN32)
    set(PLATFORM_SUFFIX "_win32")
else()
    set(PLATFORM_SUFFIX "_unix")
endif()

if(EXISTS "/.flatpak-info")
    set(CONTOUR_WITH_UTEMPTER OFF)
endif()

set(vtpty_LIBRARIES crispy::core Microsoft.GSL::GSL)

set(vtpty_SOURCES
    MockPty.cpp
    MockViewPty.cpp
    Process${PLATFORM_SUFFIX}.cpp
    Pty.cpp
)

set(vtpty_HEADERS
    MockPty.h
    MockViewPty.h
    PageSize.h
    Process.h
    Pty.h
)

set(_include_SshSession_module FALSE)
if(WIN32)
    find_package(Libssh2 CONFIG REQUIRED)
    set(_include_SshSession_module TRUE)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 libssh2)
    if(LIBSSH2_FOUND)
        set(_include_SshSession_module TRUE)
    endif()
endif()
if(_include_SshSession_module)
    add_compile_definitions(VTPTY_LIBSSH2)
    list(APPEND vtpty_SOURCES SshSession.cpp)
    list(APPEND vtpty_HEADERS SshSession.h)
    message(STATUS "[vtpty] Builtin-SSH support enabled")
else()
    message(STATUS "[vtpty] Builtin-SSH support disabled")
endif()

if(LINUX)
    if(CONTOUR_WITH_UTEMPTER)
        set(vtpty_LIBRARIES ${vtpty_LIBRARIES} utempter)
    endif()
endif()

if(UNIX)
    include(CheckFunctionExists)
    check_function_exists(close_range HAVE_CLOSE_RANGE)

    list(APPEND vtpty_LIBRARIES util)
    list(APPEND vtpty_SOURCES UnixPty.cpp UnixUtils.cpp)
    list(APPEND vtpty_SOURCES UnixPty.h UnixUtils.h)
else()
    list(APPEND vtpty_SOURCES ConPty.cpp)
    list(APPEND vtpty_HEADERS ConPty.h)
    #TODO: list(APPEND vtpty_SOURCES WinPty.cpp)
endif()

add_library(vtpty STATIC ${vtpty_SOURCES} ${vtpty_HEADERS})
set_target_properties(vtpty PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
if(CONTOUR_WITH_UTEMPTER)
    target_compile_definitions(vtpty PRIVATE UTEMPTER=1)
endif()
if(HAVE_CLOSE_RANGE)
    target_compile_definitions(vtpty PRIVATE HAVE_CLOSE_RANGE)
endif()
if(_include_SshSession_module)
    target_compile_definitions(vtpty PUBLIC VTPTY_LIBSSH2=1)
endif()
if(LIBSSH2_FOUND)
    target_compile_options(vtpty PRIVATE ${LIBSSH2_CFLAGS_OTHER})
    target_include_directories(vtpty PRIVATE ${LIBSSH2_INCLUDE_DIRS})
    target_link_libraries(vtpty PRIVATE ${LIBSSH2_LINK_LIBRARIES})
elseif(WIN32)
    target_link_libraries(vtpty PRIVATE Libssh2::libssh2)
endif()
target_include_directories(vtpty PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
target_link_libraries(vtpty PUBLIC ${vtpty_LIBRARIES})

if(WIN32)
    option(CONTOUR_EMBED_OPENCONSOLE "Embed OpenConsole" ON)
    if(NOT CONTOUR_EMBED_OPENCONSOLE)
        message(STATUS "[vtpty] OpenConsole embedding disabled")
    else()
        message(STATUS "[vtpty] OpenConsole embedding enabled")

        set(OPENCONSOLE_VERSION "1.24.3504.0")
        set(OPENCONSOLE_VERSION_ALT "1.24.251216004-preview")

        set(OPENCONSOLE_URL "https://github.com/microsoft/terminal/releases/download/v${OPENCONSOLE_VERSION}/Microsoft.Windows.Console.ConPTY.${OPENCONSOLE_VERSION_ALT}.nupkg")
        set(OPENCONSOLE_DEST "${CMAKE_BINARY_DIR}/_deps/OpenConsole-${OPENCONSOLE_VERSION}")
        set(OPENCONSOLE_ZIP "${OPENCONSOLE_DEST}/OpenConsole.zip")

        set(nuget_arch "x64")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
            set(nuget_arch "arm64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM")
            set(nuget_arch "arm")
        endif()
        set(OPENCONSOLE_EXE "${OPENCONSOLE_DEST}/build/native/runtimes/${nuget_arch}/OpenConsole.exe")
        set(CONPTY_DLL "${OPENCONSOLE_DEST}/runtimes/win-${nuget_arch}/native/Conpty.dll")

        if(NOT EXISTS "${OPENCONSOLE_EXE}")
            file(MAKE_DIRECTORY "${OPENCONSOLE_DEST}")
            message(STATUS "[vtpty] Downloading OpenConsole ConPTY v${OPENCONSOLE_VERSION}...")
            file(DOWNLOAD "${OPENCONSOLE_URL}" "${OPENCONSOLE_ZIP}" SHOW_PROGRESS)
            message(STATUS "[vtpty] Extracting OpenConsole ConPTY...")
            execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${OPENCONSOLE_ZIP}" WORKING_DIRECTORY "${OPENCONSOLE_DEST}")
        endif()

        if(NOT DEFINED CMAKE_INSTALL_BINDIR)
            set(CMAKE_INSTALL_BINDIR bin)
        endif()

        install(FILES "${OPENCONSOLE_EXE}" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT contour)
        install(FILES "${CONPTY_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT contour)

        if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
            add_custom_command(TARGET vtpty POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENCONSOLE_EXE}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/"
            )
            add_custom_command(TARGET vtpty POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CONPTY_DLL}"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/"
            )
        endif()
    endif()
endif()
