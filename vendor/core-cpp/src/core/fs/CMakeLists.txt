# SPDX-License-Identifier: Apache-2.0

# core::fs: the descriptor-relative file system. Every operation is a core::async::Task, every
# blocking call runs on an injected executor through core::async::offload, and a path is resolved
# beneath the descriptor it is relative to, never by the kernel from the root.
#
# What differs per OS is data, or a few calls behind posix/PosixSystem.hpp: the table that classifies
# a system error code, the name rules of the native PathPolicy, how a stat's times are spelled, how
# to advise, sync and append. Each is compiled once, and the platform is asked once, here, as
# src/core/net/CMakeLists.txt does for DefaultBackend.cpp: SOURCES_POSIX and SOURCES_BSD both apply
# on Apple and the BSDs (and SOURCES_LINUX with SOURCES_POSIX on Linux), so two files listed per
# directory would both define the same function.
#   - errno table: the BSDs and Apple report a symbolic link refused by O_NOFOLLOW differently
#     (FreeBSD answers EMLINK).
#   - PosixSystem.cpp: Apple spells the times st_atimespec, advises through F_RDADVISE and syncs
#     through F_FULLFSYNC (darwin/); every other POSIX system has st_atim, posix_fadvise and
#     fdatasync (posix/).
#   - Append.cpp: Linux appends in place with pwritev2(RWF_APPEND); every other POSIX system opens a
#     second, O_APPEND handle.
#   - BeneathDefault.cpp: what BeneathStrategy::Best stands for. Linux resolves beneath with
#     openat2(RESOLVE_BENEATH) where the kernel has it (linux/); every other system walks one
#     component at a time (posix/, windows/).
#   - windows/: NtCreateFile relative to a held handle opens each component, NtSetInformationFile
#     renames and links beneath one, both from ntdll, which the module links PRIVATE on Windows.
#   - Lease.cpp: an O_NONBLOCK open of a regular file under a Linux lease answers EWOULDBLOCK, and is
#     made again, waiting, through a path handle checked to be a regular file (linux/); no other
#     system has leases (posix/).
#   - DirectoryRead.cpp: a directory stream reads Linux's getdents64 records into a fixed buffer
#     (linux/); every other POSIX system reads through fdopendir and readdir (posix/).
#   - AccessFacts.cpp: who may reach an object, read from its handle. Linux reads the fstat and
#     judges nothing on an NFS or SMB mount or an object with an NFSv4 ACL (linux/); OpenBSD and
#     DragonFly need only the fstat (posix/); NetBSD, whose ACLs are not read, and any other POSIX
#     system judge nothing (posix/UnknownAccessFacts.cpp); the mode caps what they inherit
#     (posix/ModeCappedCreation.cpp). FreeBSD reads an NFSv4 ACL
#     (bsd/), macOS an extended one (darwin/), Windows the owner and DACL (windows/, beside
#     OwnerSecurity.cpp, the descriptor owner-only creations are made with). Each fills the same
#     AccessFacts, and AccessScope.cpp decides from them on every OS.
#   - MountBoundaries.cpp: what a relation walk that leaves a mount reads of it. Linux reads the
#     mount's device, root and type in mountinfo (linux/); macOS and the BSDs read the mount and its
#     type from fstatfs (bsd/), and whether the step left from its root from MountExit.cpp: macOS's
#     firmlinks leave below it (darwin/), the other BSDs only at it (bsd/); any other POSIX system
#     knows the device alone (posix/). Windows
#     walks by final path and needs none (windows/Ancestry.cpp).
if(WIN32)
    set(_coreCppFsPlatform windows/Win32ErrorTable.cpp windows/NtStatusTable.cpp windows/BeneathDefault.cpp windows/AccessFacts.cpp)
    set(_coreCppFsPrivateLibs ntdll advapi32)
elseif(APPLE)
    set(_coreCppFsPlatform bsd/ErrnoTable.cpp darwin/PosixSystem.cpp posix/Append.cpp posix/BeneathDefault.cpp posix/DirectoryRead.cpp posix/Lease.cpp darwin/AccessFacts.cpp bsd/MountBoundaries.cpp darwin/MountExit.cpp)
elseif(CORE_CPP_PLATFORM_BSD)
    set(_coreCppFsPlatform bsd/ErrnoTable.cpp posix/PosixSystem.cpp posix/Append.cpp posix/BeneathDefault.cpp posix/DirectoryRead.cpp posix/Lease.cpp bsd/MountBoundaries.cpp bsd/MountExit.cpp)
    # FreeBSD's NFSv4 ACLs are read; NetBSD's are not, so nothing is judged there; OpenBSD and
    # DragonFly have none beyond what the mode bounds.
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        list(APPEND _coreCppFsPlatform bsd/AccessFacts.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
        list(APPEND _coreCppFsPlatform posix/UnknownAccessFacts.cpp posix/ModeCappedCreation.cpp)
    else()
        list(APPEND _coreCppFsPlatform posix/AccessFacts.cpp posix/ModeCappedCreation.cpp)
    endif()
elseif(LINUX)
    set(_coreCppFsPlatform posix/ErrnoTable.cpp posix/PosixSystem.cpp linux/Append.cpp linux/BeneathDefault.cpp linux/DirectoryRead.cpp linux/Lease.cpp linux/AccessFacts.cpp posix/ModeCappedCreation.cpp linux/MountBoundaries.cpp)
else()
    set(_coreCppFsPlatform posix/ErrnoTable.cpp posix/PosixSystem.cpp posix/Append.cpp posix/BeneathDefault.cpp posix/DirectoryRead.cpp posix/Lease.cpp posix/UnknownAccessFacts.cpp posix/ModeCappedCreation.cpp posix/MountBoundaries.cpp)
endif()

# The openat2 path spells the call's ABI itself (linux/Openat2.hpp), so no UAPI 5.6 headers or glibc
# 2.32 are needed to build it; whether the running kernel has the call is probed at run time, per
# file system. Its test checks the spelling against <linux/openat2.h>.

core_cpp_add_module(fs KIND STATIC
    HEADERS
        AccessScope.hpp
        Descriptor.hpp
        FsError.hpp
        NativeFileSystem.hpp
        PathPolicy.hpp
        Rights.hpp
        testing/InMemoryDescriptorFileSystem.hpp
    SOURCES
        AccessScope.cpp
        FsError.cpp
        PathPolicy.cpp
        Rights.cpp
        detail/Ancestry.cpp
        detail/Ancestry.hpp
        detail/DirectoryStream.cpp
        detail/DirectoryStream.hpp
        detail/NativeSupport.cpp
        detail/NativeSupport.hpp
        detail/OwnedHandle.hpp
        detail/PathWalk.cpp
        detail/PathWalk.hpp
        detail/ResolveBeneath.hpp
        testing/InMemoryDescriptorFileSystem.cpp
        ${_coreCppFsPlatform}
    SOURCES_POSIX
        posix/Ancestry.cpp
        posix/DirectoryEntries.cpp
        posix/DirectoryRead.hpp
        posix/MountBoundaries.hpp
        posix/ErrnoRows.hpp
        posix/NativePathPolicy.cpp
        posix/OwnerOnlyCreation.cpp
        posix/OwnerOnlyCreation.hpp
        posix/PosixDescriptor.cpp
        posix/PosixPathWalk.cpp
        posix/PosixPathWalk.hpp
        posix/PosixSystem.hpp
    SOURCES_LINUX
        linux/Append.hpp
        linux/DirectoryRead.hpp
        linux/MountInfo.hpp
        linux/Openat2.cpp
        linux/Openat2.hpp
    SOURCES_WINDOWS
        windows/Ancestry.cpp
        windows/DirectoryRecords.cpp
        windows/DirectoryRecords.hpp
        windows/NativePathPolicy.cpp
        windows/AccessFacts.hpp
        windows/NtFile.hpp
        windows/OwnerSecurity.cpp
        windows/OwnerSecurity.hpp
        windows/ReparsePoint.cpp
        windows/ReparsePoint.hpp
        windows/StableFileIdRows.cpp
        windows/StableFileIdRows.hpp
        windows/WindowsDescriptor.cpp
        windows/WindowsPathWalk.cpp
        windows/WindowsPathWalk.hpp
    PUBLIC_LIBS core::base core::async core::platform
    PRIVATE_LIBS ${_coreCppFsPrivateLibs})

# posix/Append.cpp, posix/BeneathDefault.cpp, posix/DirectoryRead.cpp and posix/Lease.cpp are what Apple and
# the BSDs append, resolve, read directories and open leased files with,
# so a Linux build would never compile them: parse them there too, so a rename or a dropped include
# shows up before a macOS job does. posix/MountBoundaries.cpp, which only a POSIX system that is
# neither Linux, Apple nor a BSD builds, is parsed with them.
if(LINUX)
    core_cpp_add_unbuilt_source_check(fs-posix
        SOURCES posix/Append.cpp posix/BeneathDefault.cpp posix/DirectoryRead.cpp posix/Lease.cpp
                posix/MountBoundaries.cpp
        PART_OF fs
        USAGE core::base core::async core::platform)
endif()

# How a test plants an inheritable ACL through the host: per OS, as the readers are chosen above.
if(APPLE)
    set(_coreCppFsInheritableAcl darwin/InheritableAcl_test.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_coreCppFsInheritableAcl bsd/InheritableAcl_test.cpp)
else()
    set(_coreCppFsInheritableAcl posix/InheritableAcl_test.cpp)
endif()

# The relation across macOS's own volumes: a firmlink leaves the Data volume below its root, and a
# disk image mounted beneath a grant. On FreeBSD, nullfs and tmpfs mounts beneath a grant.
set(_coreCppFsMountTests)
if(APPLE)
    set(_coreCppFsMountTests darwin/Relation_test.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(_coreCppFsMountTests bsd/Relation_test.cpp)
endif()

# One binary: every case links core::fs and nothing the module's row does not list.
# DescriptorTestSupport.hpp is the test-only fixture the descriptor cases share.
core_cpp_add_test(fs
    SOURCES
        AccessScope_test.cpp
        DescriptorContract_test.cpp
        DescriptorEscape_test.cpp
        DescriptorTestSupport.hpp
        DirectoryStream_test.cpp
        FsError_test.cpp
        NativeDescriptor_test.cpp
        PathPolicy_test.cpp
        Relation_test.cpp
        detail/PathWalk_test.cpp
        testing/InMemoryDescriptorFileSystem_test.cpp
    SOURCES_POSIX
        posix/DescriptorTestSupport.cpp
        posix/DirectoryEntries_test.cpp
        posix/MountBoundaries_test.cpp
        posix/InheritableAcl.hpp
        ${_coreCppFsInheritableAcl}
        ${_coreCppFsMountTests}
        posix/OwnerOnly_test.cpp
        posix/PosixDescriptor_test.cpp
        posix/PosixPathWalk_test.cpp
    SOURCES_LINUX
        linux/Append_test.cpp
        linux/DescriptorEscape_test.cpp
        linux/DirectoryRead_test.cpp
        linux/Lease_test.cpp
        linux/MountInfo_test.cpp
        linux/Openat2_test.cpp
        linux/Relation_test.cpp
    SOURCES_WINDOWS
        windows/DescriptorEscape_test.cpp
        windows/DescriptorTestSupport.cpp
        windows/DirectoryRecords_test.cpp
        windows/OwnerOnly_test.cpp
        windows/Relation_test.cpp
        windows/VirtualVolumeTestSupport.hpp
        windows/WindowsPathWalk_test.cpp)

# What the last owner of a handle does when its queued close cannot be allocated: a binary of its own
# because it replaces the global allocation functions, which would reach every case linked beside
# it. POSIX only, where a closed descriptor is plain to see. Not under a sanitizer, whose runtime
# defines those functions itself (the link refuses a second definition).
if(NOT CORE_CPP_SANITIZERS AND NOT WIN32)
    core_cpp_add_test(fs NAME fs-alloc
        SOURCES posix/OwnedHandleAllocation_test.cpp ../testing/ReplacedGlobalAllocation.cpp)
    # What a directory stream holds while it reads a large directory, counted the same way.
    core_cpp_add_test(fs NAME fs-memory
        SOURCES posix/DirectoryStreamMemory_test.cpp DescriptorTestSupport.hpp ../testing/ReplacedGlobalAllocation.cpp)
    # What a relation holds while it walks, counted the same way.
    core_cpp_add_test(fs NAME fs-relation-memory
        SOURCES RelationMemory_test.cpp DescriptorTestSupport.hpp ../testing/ReplacedGlobalAllocation.cpp)
endif()
