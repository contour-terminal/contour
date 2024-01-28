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
    set(FLATPAK TRUE)
endif()

set(vtpty_LIBRARIES crispy::core fmt::fmt-header-only Microsoft.GSL::GSL)

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
    if(NOT FLATPAK)
        set(vtpty_LIBRARIES ${vtpty_LIBRARIES} utempter)
    endif()
endif()

if(UNIX)
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
if(FLATPAK)
    target_compile_definitions(vtpty PRIVATE FLATPAK=1)
endif()
if(_include_SshSession_module)
    target_compile_definitions(vtpty PUBLIC VTPTY_LIBSSH2=1)
endif()
if(LIBSSH2_FOUND)
    target_compile_options(vtpty PRIVATE ${LIBSSH2_CFLAGS_OTHER})
    target_include_directories(vtpty PRIVATE ${LIBSSH2_INCLUDE_DIRS})
    target_link_libraries(vtpty PRIVATE ${LIBSSH2_LINK_LIBRARIES})
elseif(WIN32)
    target_link_libraries(vtpty PRIVATE $<IF:$<TARGET_EXISTS:Libssh2::libssh2_shared>,Libssh2::libssh2_shared,Libssh2::libssh2_static>)
endif()
target_include_directories(vtpty PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)
target_link_libraries(vtpty PUBLIC ${vtpty_LIBRARIES})
