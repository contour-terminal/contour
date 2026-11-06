// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Starting a daemon from a handover image: what the new process image keeps of the descriptors it
/// inherited, and what it tells a client of a session whose screen it could not bring back.
///
/// The sessions themselves are taken over by the host (SessionHostConfig::handover).

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/HandoverMemory.hpp>

namespace vthost::handover
{

/// The title of the notice a client is shown for a session restored without its screen.
inline constexpr std::string_view UnrestoredScreenTitle = "Contour";

/// The notice a client is shown for a session restored without its screen.
inline constexpr std::string_view UnrestoredScreenNotice =
    "Contour could not restore this pane's screen after the upgrade; the program is still running";

/// Reads the image a previous image of this daemon handed over, and decodes it. Closes @p memoryHandle
/// however that goes: the image is all it was for.
/// @param memory Where the image was written (makeNativeHandoverMemory()).
/// @param memoryHandle The memory object: the descriptor `--handover=fd=` names on POSIX, the mapping
///        `--handover=handles=` names on Windows.
/// @return The image; or why it could not be read or decoded.
[[nodiscard]] std::expected<HandoverImage, HandoverError> readHandover(HandoverMemory& memory,
                                                                       vtpty::CrossingHandle memoryHandle);

/// @param image A handover image.
/// @return Every descriptor or handle @p image names -- each session's PTY (its master and fast-pipe ends on
///         POSIX, its pseudoconsole on Windows) and child (Windows), each listener, the image lease --
///         sorted, each once, none none: what the old image keeps open across its exec on POSIX, and what the
///         successor inherits on Windows.
[[nodiscard]] std::vector<vtpty::CrossingHandle> handlesNamedBy(HandoverImage const& image);

#ifndef _WIN32
// What a daemon exec'd from a handover image does with the descriptors it inherited (@see
// makeNativeImageReceiver()). POSIX alone: no descriptor crosses an exec on Windows, and a handle does not
// fit an int.

/// Closes every listening socket this process holds: what a daemon whose handover image could not be read
/// does before it binds its own, which the previous image's -- inherited, but no longer known by name --
/// would otherwise refuse as taken. Every other inherited descriptor is left open, so that no program the
/// previous image hosted is hung up on.
/// @return How many were closed.
std::size_t closeListeningSockets();

/// @param image A decoded handover image.
/// @return Every descriptor the new image keeps: standard input, output and error, and each one @p image
///         names -- every session's PTY master and fast-pipe ends, every listener, the image lease --
///         ascending, each once. What a closeDescriptorsExcept() pass is handed.
[[nodiscard]] std::vector<int> descriptorsToKeep(HandoverImage const& image);

/// Closes every descriptor of this process but @p keep, the way closefrom(2) closes all from a number on:
/// close_range(2) on Linux, closefrom(3) on the BSDs, and on macOS a loop up to the descriptor limit
/// (RLIMIT_NOFILE, bounded). What a process started from a handover image runs first, so that nothing the
/// image does not name -- a descriptor the old image leaked across the exec -- stays open in it. Allocates
/// nothing, so a forked child may call it too.
/// @param keep The descriptors to leave open: ascending, each once, none negative (Require).
void closeDescriptorsExcept(std::span<int const> keep);
#endif

} // namespace vthost::handover
