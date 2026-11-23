// SPDX-License-Identifier: Apache-2.0
//
// A daemon-world extension's roots, read out of its configuration: what `root <label> = config:<pointer>`
// names, expanded and resolved once by the host, and the digest a grant is bound to.
#pragma once

#include <vtpty/PathStyle.hpp>

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
    NotAbsolute,     ///< A directory is neither absolute nor `~` or `~/…` (`~user` is not expanded); on
                     ///< Windows `~\…` is accepted too, and a drive-relative `C:x`, a rooted `\x` without a
                     ///< drive and a UNC path without a share are refused.
    EqualRoots,      ///< Two directories of the roots resolve to the same one.
    NoHome,          ///< A directory is `~` or `~/…`, and the user's home directory is not known.
    DevicePath,      ///< A directory is a Win32 device or verbatim path: `\\?\`, `\\.\`, `\??\`.
    NotPlainName ///< A component of a directory is one Windows spells otherwise: `.`, `..`, empty, a stream,
                 ///< a device name, a trailing dot or space.
};

/// Reads the text values a root's pointer names.
/// @param configJson The extension's configuration, as JSON.
/// @param pointer Slash-separated object keys; a `*` component walks every element of a list.
/// @return The values, in document order; or why they could not be read.
[[nodiscard]] std::expected<std::vector<std::string>, RootPointerError> rootValuesOf(
    std::string_view configJson, std::string_view pointer);

/// Spells a configured directory in @p style before it is resolved. Posix: `~` and `~/…` become @p home and
/// home/…, and the result must start with `/`; nothing else is normalised. Windows: `~`, `~\…` and `~/…` are
/// expanded alike; every `/` is made `\`, the drive letter upper-cased, and trailing separators dropped
/// except a drive root's own; the result must be `X:\…` or `\\server\share…`.
/// @param raw The directory as configured.
/// @param home The user's home directory.
/// @param style The grammar @p raw is spelled in.
/// @return The spelling; `NoHome` for `~…` while @p home is empty; `NotAbsolute` (`~user`, relative,
///         drive-relative `C:x`, rooted without a drive `\x`, a UNC path without a share, and on Windows any
///         spelling isNormalisedAbsolutePath() refuses, one longer than MaxPathArgumentBytes among them);
///         `DevicePath` (`\\?\`, `\\.\`, `\??\`); `NotPlainName` (a component vtpty::isPlainWindowsName()
///         refuses).
[[nodiscard]] std::expected<std::string, RootPointerError> rootSpellingOf(std::string_view raw,
                                                                          std::string_view home,
                                                                          vtpty::PathStyle style);

/// Resolves one configured directory: rootSpellingOf(), then weaklyCanonical, so links are resolved as far
/// as the path exists. Posix: the result has no trailing `/`. Windows: the canonical path is stripped of
/// `\\?\` and spelled by rootSpellingOf() again; a canonical form outside the grammar (`\\?\Volume{…}\`) is
/// refused.
/// @param raw The directory as configured.
/// @param home The user's home directory.
/// @param fileSystem Where links are resolved.
/// @param style The grammar @p raw, and @p fileSystem's paths, are spelled in.
/// @return The resolved directory; rootSpellingOf()'s error; or `NotAbsolute` for a canonical form outside
///         the grammar.
[[nodiscard]] std::expected<std::string, RootPointerError> resolveRootPath(
    std::string_view raw,
    std::string_view home,
    core::platform::FileSystem const& fileSystem,
    vtpty::PathStyle style);

/// Resolves every root a manifest declares out of the extension's configuration.
/// @param manifest The extension's manifest.
/// @param configJson Its configuration, as JSON.
/// @param home The user's home directory.
/// @param fileSystem Where links are resolved.
/// @param style The grammar the directories are spelled, and compared, in.
/// @return One entry per root, in declaration order; or the first error, `EqualRoots` when two of the
///         directories resolve to the same one.
[[nodiscard]] std::expected<std::vector<ResolvedRoot>, RootPointerError> resolveRoots(
    Manifest const& manifest,
    std::string_view configJson,
    std::string_view home,
    core::platform::FileSystem const& fileSystem,
    vtpty::PathStyle style);

/// The digest a daemon-world grant is recorded for: the manifest and the directories it was granted over,
/// so that a moved directory or another repository asks again.
/// @param manifestDigest The manifest's digest.
/// @param roots The resolved roots.
/// @return The lower-case hex SHA-256 of `<manifestDigest>\n`, then `<label>\t<path>\n` per resolved path,
///         in declaration order.
[[nodiscard]] std::string grantDigestOf(std::string_view manifestDigest, std::span<ResolvedRoot const> roots);

} // namespace vtextension
