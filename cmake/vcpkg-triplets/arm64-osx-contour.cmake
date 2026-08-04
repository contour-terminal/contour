# SPDX-License-Identifier: Apache-2.0
#
# vcpkg triplet for Contour's macOS release packaging.
#
# The point of this file is VCPKG_OSX_DEPLOYMENT_TARGET. Homebrew ships one prebuilt
# bottle per macOS release and installs the one matching the build machine, so a bundle
# assembled from Homebrew dylibs inherits the *builder's* macOS as its minimum -- macOS 26
# on a developer's up-to-date Mac. Building the same libraries from source against a
# pinned deployment target puts the floor where we decide, and keeps it there no matter
# who builds. `scripts/macos-bundle.py verify --max-minimum-os` enforces the result.
#
# @see docs/internals/macos-code-signing.md

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)

# Dynamic, deliberately. Static linkage would make the floor a non-issue and shrink the
# bundle, but cairo is LGPL-2.1/MPL: statically linking it into an Apache-2.0 binary we
# distribute would oblige us to ship relinkable objects. Dynamic linking sidesteps that
# entirely, and the deployment target below already solves the floor.
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)

# Must stay in sync with CMAKE_OSX_DEPLOYMENT_TARGET in the top-level CMakeLists.txt --
# 13.3 is the first macOS whose libc++ exports the floating-point std::to_chars that
# <format> needs.
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)
set(VCPKG_OSX_ARCHITECTURES arm64)
