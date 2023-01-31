if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    set(LINUX TRUE)
endif()

# Platform suffix can be either _win32 or _unix.
if(WIN32)
    set(PLATFORM_SUFFIX "_win32")
else()
    set(PLATFORM_SUFFIX "_unix")
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

if(LINUX)
    set(vtpty_HEADERS ${vtpty_HEADERS} LinuxPty.h)
    set(vtpty_SOURCES ${vtpty_SOURCES} LinuxPty.cpp)
endif()

if(UNIX)
    list(APPEND vtpty_LIBRARIES util)
    list(APPEND vtpty_SOURCES UnixPty.cpp UnixUtils.h)
else()
    list(APPEND vtpty_SOURCES ConPty.cpp)
    #TODO: list(APPEND vtpty_SOURCES WinPty.cpp)
endif()

add_library(vtpty STATIC ${vtpty_SOURCES} ${vtpty_HEADERS})
set_target_properties(vtpty PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
target_include_directories(vtpty PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vtpty PUBLIC ${vtpty_LIBRARIES})
