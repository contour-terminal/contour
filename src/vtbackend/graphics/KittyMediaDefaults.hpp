// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/graphics/KittyMediaPolicy.hpp>
#include <vtbackend/graphics/KittyMediumSource.hpp>

#include <core/Environment.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

/// What a composition root grants a local session's kitty file media until configuration supplies its own
/// (per profile: media on/off and the allow-list). Unlike KittyMediaPolicy, these read the platform: they are
/// called where the root is wired, never from the terminal.
namespace vtbackend::kitty_graphics
{

/// The temp directories of the platform, canonical where they exist: `$TMPDIR` (when absolute), plus `/tmp`
/// and `/dev/shm` on Linux, or `/private/tmp` and the per-user `confstr(_CS_DARWIN_USER_TEMP_DIR)` on macOS.
/// None on Windows, which reads no file medium.
/// @param env The environment of the process that parses: the daemon's `$TMPDIR` can differ from the app's.
/// @return The roots, without duplicates.
[[nodiscard]] std::vector<std::filesystem::path> platformTemporaryRoots(core::Environment const& env);

/// The policy by default: file media enabled (disabled on Windows), the platform's temp roots, the allow-list
/// `~/.claude/plugins/data`, and the size limit of one direct transmission (MaxChunkedPayloadSize).
/// @param env The parsing process's environment, for platformTemporaryRoots().
/// @param homeDirectory The user's home directory, which the allow-list is below; none when it is not
///        absolute.
/// @param userId The reading user, who must own every file read.
/// @return The policy.
[[nodiscard]] MediaPolicy defaultMediaPolicy(core::Environment const& env,
                                             std::filesystem::path const& homeDirectory,
                                             uint32_t userId);

/// @return The id of the user this process runs as: the owner every file read must have. 0 on Windows.
[[nodiscard]] uint32_t currentUserId() noexcept;

/// @return The platform's medium source: PosixMediumSource on Linux and macOS, RefusingMediumSource on
///         Windows.
[[nodiscard]] std::unique_ptr<KittyMediumSource const> makePlatformMediumSource();

} // namespace vtbackend::kitty_graphics
