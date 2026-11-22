// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/graphics/KittyMediaPolicy.hpp>
#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <core/Environment.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

/// What a composition root grants a local session's kitty file media: the configuration's say (MediaSettings)
/// completed by the platform's. Unlike KittyMediaPolicy, these read the platform: they are called where the
/// root is wired, never from the terminal.
namespace vtbackend::kitty_graphics
{

/// The temp directories of the platform, each both as spelled and canonical (where it exists): a reader
/// checks the requested path before resolving it, and the canonical one after (macOS spells `/var/folders`
/// what is
/// `/private/var/folders`). `$TMPDIR` (when absolute), plus `/tmp` on Linux, or `/private/tmp` and the
/// per-user `confstr(_CS_DARWIN_USER_TEMP_DIR)` on macOS. None on Windows, which reads no file medium.
/// @param env The environment of the process that parses: the daemon's `$TMPDIR` can differ from the app's.
/// @return The roots, without duplicates.
[[nodiscard]] std::vector<std::filesystem::path> platformTemporaryRoots(core::Environment const& env);

/// The platform's roots of `t=t` alone: `/dev/shm` on Linux, as kitty allows; none elsewhere.
/// @return The roots.
[[nodiscard]] std::vector<std::filesystem::path> platformTemporaryFileOnlyRoots();

/// What the configuration decides of the media policy; the platform supplies the rest (mediaPolicy()).
struct MediaSettings
{
    /// Whether file media are read at all. Refused on Windows whatever this says.
    FileTransmission transmission = FileTransmission::Disabled;
    /// Further roots `t=f` may read below, home-resolved by the configuration. A relative one is dropped.
    std::vector<std::filesystem::path> allowList;
};

/// The policy the configuration's @p settings make on this platform: its temp roots, the allow-list, and the
/// size limit of one direct transmission (MaxChunkedPayloadSize). File media stay disabled on Windows. A root
/// that would open the whole home directory -- `/`, the home directory itself or any directory above it,
/// judged by canonical forms (a `$TMPDIR=$HOME`, an allow-list entry of `~` or one linked to it) -- is
/// dropped.
/// @param env The parsing process's environment, for platformTemporaryRoots().
/// @param settings What the configuration says.
/// @param homeDirectory The user's home directory, which no root may open; ignored when not absolute.
/// @param userId The reading user, who must own every file read.
/// @return The policy.
[[nodiscard]] MediaPolicy mediaPolicy(core::Environment const& env,
                                      MediaSettings const& settings,
                                      std::filesystem::path const& homeDirectory,
                                      uint32_t userId);

/// @return The id of the user this process runs as: the owner every file read must have. 0 on Windows.
[[nodiscard]] uint32_t currentUserId() noexcept;

/// @return The platform's medium source: PosixMediumSource on Linux and macOS, RefusingMediumSource on
///         Windows.
[[nodiscard]] std::unique_ptr<KittyMediumSource const> makePlatformMediumSource();

} // namespace vtbackend::kitty_graphics
