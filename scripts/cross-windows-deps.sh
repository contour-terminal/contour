#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Fetches the C libraries a Linux -> Windows cross build links against (OpenSSL, libssh2, zlib, FreeType,
# HarfBuzz, cairo and what they pull in) as Fedora's prebuilt mingw64 packages, and lays them out as a
# prefix the clang-cl toolchain (cmake/toolchains/windows-clangcl-cross.cmake) finds them in.
#
# Usage: scripts/cross-windows-deps.sh [PREFIX]   (default: out/windows-cross-deps)
#
# MinGW-built DLLs are usable from MSVC-ABI code because these are C libraries: the import libraries
# are GNU-style (.dll.a), which lld-link reads as they are, so they are only copied under the names CMake's
# find modules look for on MSVC. The C++ dependencies (Catch2, yaml-cpp, GSL, glaze, libunicode, ...) are
# not taken from here: CPM builds them with clang-cl, as they have no stable ABI across compilers.
#
# Needs dnf (any Fedora; no root, packages are only downloaded), rpm2cpio and cpio.
#
# @see docs/internals/windows-cross-build.md
set -euo pipefail

readonly PACKAGES=(mingw64-openssl mingw64-libssh2 mingw64-zlib mingw64-freetype mingw64-harfbuzz mingw64-cairo)

prefix="$(realpath -m "${1:-out/windows-cross-deps}")"
work="$(mktemp -d)"
trap 'chmod -R u+w "$work"; rm -rf "$work"' EXIT

for tool in dnf rpm2cpio cpio; do
    command -v "$tool" >/dev/null || { echo "$0: $tool is required" >&2; exit 1; }
done

echo "Downloading ${PACKAGES[*]} and their dependencies"
(cd "$work" && dnf download --quiet --resolve --alldeps --arch noarch "${PACKAGES[@]}")

mkdir -p "$work/root"
# --alldeps also names what the host has installed, so it does not depend on the host; only the
# mingw64 packages are the target's.
for rpm in "$work"/mingw64-*.rpm; do
    (cd "$work/root" && rpm2cpio "$rpm" | cpio -idm --quiet)
done

rm -rf "$prefix"
mkdir -p "$prefix"
cp -a "$work/root/usr/x86_64-w64-mingw32/sys-root/mingw/." "$prefix/"

# zconf.h as configured for MinGW includes <unistd.h>, which MSVC's runtime has not; the zlib built for
# MSVC ships the same header without that line.
sed -i 's/^#define HAVE_UNISTD_H 1$//' "$prefix/include/zconf.h"

# pkg-config files name the prefix they were built for, some of them in more places than prefix=.
sed -i "s|/usr/x86_64-w64-mingw32/sys-root/mingw|$prefix|g" "$prefix"/lib/pkgconfig/*.pc

# On MSVC, CMake looks for foo.lib (FindZLIB, FindFreetype, harfbuzz's config) or libfoo.lib (FindOpenSSL).
for import in "$prefix"/lib/lib*.dll.a; do
    name="$(basename "$import" .dll.a)"
    cp "$import" "$prefix/lib/$name.lib"
    cp "$import" "$prefix/lib/${name#lib}.lib"
done

# harfbuzz's config spells its DLL with the target's library prefix, which is empty on MSVC; MinGW's is
# libharfbuzz-0.dll. Where the DLL is matters to whatever stages a target's runtime DLLs beside it.
sed -i 's|/bin/${CMAKE_SHARED_LIBRARY_PREFIX}${library_name}|/bin/lib${library_name}|' \
    "$prefix/lib/cmake/harfbuzz/harfbuzz-config.cmake"

# libssh2 installs no CMake package for MinGW; vtpty asks for vcpkg's Libssh2::libssh2.
mkdir -p "$prefix/lib/cmake/Libssh2"
cat >"$prefix/lib/cmake/Libssh2/Libssh2Config.cmake" <<'EOF'
# Written by scripts/cross-windows-deps.sh: libssh2 as vcpkg's config names it.
get_filename_component(_libssh2Prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET Libssh2::libssh2)
    add_library(Libssh2::libssh2 SHARED IMPORTED)
    set_target_properties(Libssh2::libssh2 PROPERTIES
        IMPORTED_LOCATION "${_libssh2Prefix}/bin/libssh2-1.dll"
        IMPORTED_IMPLIB "${_libssh2Prefix}/lib/libssh2.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_libssh2Prefix}/include")
endif()
set(Libssh2_FOUND TRUE)
EOF

echo "Dependencies are in $prefix"
