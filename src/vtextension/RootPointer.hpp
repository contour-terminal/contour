// SPDX-License-Identifier: Apache-2.0
//
// A daemon-world extension's roots, read out of its configuration: what `root <label> = config:<pointer>`
// names, expanded and resolved once by the host, and the digest a grant is bound to.
#pragma once

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// Why a root's directories could not be read out of an extension's configuration.
enum class RootPointerError : uint8_t
{
    NotJson,    ///< The configuration is not a JSON document.
    MissingKey, ///< A key the pointer names is absent, or is looked up in something other than an object.
    NotAString, ///< What the pointer reaches is not text.
    StarOverNonList, ///< A `*` meets something other than a list.
    BadPointer,      ///< The pointer is empty, or has an empty component.
    NotAbsolute,     ///< A directory is neither absolute nor `~` or `~/…` (`~user` is not expanded).
    EqualRoots,      ///< Two directories of the roots resolve to the same one.
    NoHome,          ///< A directory is `~` or `~/…`, and the user's home directory is not known.
};

/// Reads the text values a root's pointer names.
/// @param configJson The extension's configuration, as JSON.
/// @param pointer Slash-separated object keys; a `*` component walks every element of a list.
/// @return The values, in document order; or why they could not be read.
[[nodiscard]] std::expected<std::vector<std::string>, RootPointerError> rootValuesOf(
    std::string_view configJson, std::string_view pointer);

/// Resolves one configured directory: `~` and `~/…` become @p home and home/…; `~user` is NotAbsolute, and
/// so is anything that does not then start with `/`. The result is weaklyCanonical: symbolic links resolved
/// as far as the path exists, and it has no trailing `/`.
/// @param raw The directory as configured.
/// @param home The user's home directory.
/// @param fileSystem Where links are resolved.
/// @return The resolved directory; `NoHome` for `~` or `~/…` while @p home is empty; or `NotAbsolute`.
[[nodiscard]] std::expected<std::string, RootPointerError> resolveRootPath(
    std::string_view raw, std::string_view home, core::platform::FileSystem const& fileSystem);

/// Resolves every root a manifest declares out of the extension's configuration.
/// @param manifest The extension's manifest.
/// @param configJson Its configuration, as JSON.
/// @param home The user's home directory.
/// @param fileSystem Where links are resolved.
/// @return One entry per root, in declaration order; or the first error, `EqualRoots` when two of the
///         directories resolve to the same one.
[[nodiscard]] std::expected<std::vector<ResolvedRoot>, RootPointerError> resolveRoots(
    Manifest const& manifest,
    std::string_view configJson,
    std::string_view home,
    core::platform::FileSystem const& fileSystem);

/// The digest a daemon-world grant is recorded for: the manifest and the directories it was granted over,
/// so that a moved directory or another repository asks again.
/// @param manifestDigest The manifest's digest.
/// @param roots The resolved roots.
/// @return The lower-case hex SHA-256 of `<manifestDigest>\n`, then `<label>\t<path>\n` per resolved path,
///         in declaration order.
[[nodiscard]] std::string grantDigestOf(std::string_view manifestDigest, std::span<ResolvedRoot const> roots);

} // namespace vtextension
