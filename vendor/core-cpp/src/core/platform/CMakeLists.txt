# SPDX-License-Identifier: Apache-2.0

# core::platform: the operating-system layer, each part behind an interface with a test double in
# testing/. Its row says PLATFORMS wasm-subset: under single-threaded Emscripten only the files in
# the SOURCES_EMSCRIPTEN lists below build (Part I §1), which is why the subset's sources appear in
# both lists.
set(_coreCppPlatformSubset CpuCounters.cpp FileUri.cpp GlobMatch.cpp PathUtils.cpp)
set(_coreCppPlatformSubsetTests
    Clock_test.cpp
    CpuCounters_test.cpp
    FileInfoProvider_test.cpp
    FileUri_test.cpp
    GlobMatch_test.cpp
    PathUtils_test.cpp
    PlatformError_test.cpp
    ProcessEnvironment_test.cpp
    StringUtils_test.cpp
    Types_test.cpp
    WorkingDirectory_test.cpp)

# The POSIX providers, which nativeProcessEnvironment(), nativeFileInfoProvider() and
# nativeWorkingDirectory() return on every POSIX system. Emscripten's libc serves them too, over its
# own environment and virtual filesystem, so they are part of the subset.
set(_coreCppPlatformPosixProviders
    posix/PosixProcessEnvironment.cpp
    posix/PosixProcessEnvironment.hpp
    posix/PosixFileInfoProvider.cpp
    posix/PosixFileInfoProvider.hpp
    posix/PosixWorkingDirectory.cpp)

# Exactly one makeDefaultCpuCounterSource() is compiled, and the platform is asked once, here, rather
# than by an `#ifdef` in logic (Ruling R40, as in src/core/net/CMakeLists.txt). A platform without a
# backend names the "unavailable" source.
if(APPLE)
    set(_coreCppPlatformCpuCounterSource darwin/DarwinCpuCounters.cpp)
elseif(LINUX)
    set(_coreCppPlatformCpuCounterSource linux/LinuxCpuCounters.cpp)
elseif(WIN32)
    set(_coreCppPlatformCpuCounterSource windows/WindowsCpuCounters.cpp)
else()
    set(_coreCppPlatformCpuCounterSource UnavailableCpuCounters.cpp)
endif()

# The same choice for makeNativeSystemPowerMonitor(); a platform is "unavailable" until its backend lands.
# Linux reads logind's PrepareForSleep over sd-bus, behind CORE_CPP_WITH_SYSTEMD; Windows registers a
# suspend/resume callback with the power manager.
set(_coreCppPlatformPowerDefinitions "")
set(_coreCppPlatformPowerIncludes "")
if(APPLE)
    set(_coreCppPlatformSystemPowerSource darwin/DarwinSystemPower.cpp)
elseif(LINUX AND CORE_CPP_WITH_SYSTEMD)
    set(_coreCppPlatformSystemPowerSource linux/LinuxSystemPower.cpp)
    # Linked by value, as Contour's vtpty does: a PkgConfig::libsystemd imported target would be
    # classified by CoreCppInstall.cmake but never re-created by the installed package config, which
    # would break a consumer's link.
    # Not REQUIRED: a missing pkg-config must stop with the message below, which names the option.
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBSYSTEMD libsystemd)
    endif()
    if(NOT PKG_CONFIG_FOUND OR NOT LIBSYSTEMD_FOUND)
        message(FATAL_ERROR "[core-cpp] CORE_CPP_WITH_SYSTEMD is ON, but libsystemd was not found "
                            "through pkg-config. Install pkg-config and libsystemd's development "
                            "package (libsystemd-dev, systemd-devel), "
                            "or configure with -DCORE_CPP_WITH_SYSTEMD=OFF.")
    endif()
    set(_coreCppPlatformPowerDefinitions CORE_CPP_HAS_SYSTEMD=1)
    set(_coreCppPlatformPowerIncludes ${LIBSYSTEMD_INCLUDE_DIRS})
elseif(WIN32)
    set(_coreCppPlatformSystemPowerSource windows/WindowsSystemPower.cpp)
else()
    set(_coreCppPlatformSystemPowerSource UnavailableSystemPower.cpp)
endif()

# Sockets back SystemPipe on Windows: a loopback pair mapped to a waitable event.
set(_coreCppPlatformLibs "")
if(APPLE)
    list(APPEND _coreCppPlatformLibs "-framework IOKit" "-framework CoreFoundation")
endif()
if(LINUX AND CORE_CPP_WITH_SYSTEMD)
    list(APPEND _coreCppPlatformLibs ${LIBSYSTEMD_LINK_LIBRARIES})
endif()
if(WIN32)
    list(APPEND _coreCppPlatformLibs ws2_32 powrprof)
endif()

core_cpp_add_module(platform KIND STATIC
    HEADERS
        Clock.hpp
        CpuCounters.hpp
        FileInfoProvider.hpp
        FileSystem.hpp
        FileUri.hpp
        GlobMatch.hpp
        MachineId.hpp
        MessageQueue.hpp
        NativeFileSystem.hpp
        PathUtils.hpp
        PlatformError.hpp
        ProcessEnvironment.hpp
        SignalHandler.hpp
        StringUtils.hpp
        SystemInfo.hpp
        SystemPipe.hpp
        SystemPower.hpp
        Types.hpp
        UserPaths.hpp
        Wakeup.hpp
        WinsockInit.hpp
        WorkingDirectory.hpp
        detail/CpuCounterProbe.hpp
        detail/LinuxPerfPlan.hpp
        detail/LogindPlan.hpp
        detail/PowerBroadcastPlan.hpp
        detail/PowerMessagePlan.hpp
        detail/UnavailableCpuCounterSource.hpp
        testing/FakeCpuCounterSource.hpp
        testing/FakeSystemPowerMonitor.hpp
        testing/InMemoryFileSystem.hpp
        testing/MockFileInfoProvider.hpp
        testing/TestProcessEnvironment.hpp
        testing/TestWorkingDirectory.hpp
    SOURCES
        ${_coreCppPlatformSubset}
        ${_coreCppPlatformCpuCounterSource}
        MachineId.cpp
        NativeFileSystem.cpp
        detail/DurableFiles.hpp
        detail/LinuxPerfPlan.cpp
        SignalHandler.cpp
        SystemInfo.cpp
        SystemPipe.cpp
        SystemPower.cpp
        ${_coreCppPlatformSystemPowerSource}
        WinsockInit.cpp
        testing/InMemoryFileSystem.cpp
    SOURCES_POSIX
        ${_coreCppPlatformPosixProviders}
        posix/PosixDurableFiles.cpp
        posix/PosixSystemInfo.cpp
    SOURCES_LINUX
        linux/LinuxWakeup.cpp
    SOURCES_BSD
        posix/PosixWakeup.cpp
    SOURCES_WINDOWS
        windows/WindowsDurableFiles.cpp
        windows/WindowsFileInfoProvider.cpp
        windows/WindowsFileInfoProvider.hpp
        windows/WindowsProcessEnvironment.cpp
        windows/WindowsProcessEnvironment.hpp
        windows/WindowsSystemInfo.cpp
        windows/WindowsTypes.cpp
        windows/WindowsWakeup.cpp
        windows/WindowsWorkingDirectory.cpp
    SOURCES_EMSCRIPTEN
        ${_coreCppPlatformSubset}
        ${_coreCppPlatformPosixProviders}
        ${_coreCppPlatformCpuCounterSource}
        SystemPower.cpp
        ${_coreCppPlatformSystemPowerSource}
    PUBLIC_LIBS core::base
    PRIVATE_LIBS ${_coreCppPlatformLibs})

if(LINUX AND CORE_CPP_WITH_SYSTEMD AND TARGET core-cpp-platform)
    target_include_directories(core-cpp-platform PRIVATE ${_coreCppPlatformPowerIncludes})
endif()

core_cpp_add_test(platform
    SOURCES
        ${_coreCppPlatformSubsetTests}
        CpuCountersNative_test.cpp
        FileSystem_test.cpp
        LinuxPerfPlan_test.cpp
        MachineId_test.cpp
        MessageQueue_test.cpp
        SignalHandler_test.cpp
        SystemInfo_test.cpp
        SystemPipe_test.cpp
        SystemPower_test.cpp
        UserPaths_test.cpp
        Wakeup_test.cpp
    DEFINITIONS ${_coreCppPlatformPowerDefinitions}
    SOURCES_WINDOWS
        windows/CpuCounters_test.cpp
        windows/DnsHostName_test.cpp
        # The Windows half of a POSIX guarantee: FD_CLOEXEC there, no inheritable handle here.
        windows/SystemPipeInheritance_test.cpp
        windows/WideEnvironment_test.cpp
    SOURCES_EMSCRIPTEN
        ${_coreCppPlatformSubsetTests})
