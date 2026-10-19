// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkTypes.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace core::platform
{
class FileSystem;
}

namespace vtbackend
{

/// Where a printed path may be, and the base each place was joined to.
struct PathCandidates
{
    /// The places, first to last: what resolveLocalPath() and a PathQuery take.
    std::vector<std::filesystem::path> paths;
    /// Parallel to @ref paths: the base a relative path was joined to; empty for an absolute or `~/` path.
    /// Not the bases themselves: one that yields no place (empty, or on Windows not UTF-8) is skipped.
    std::vector<std::string> directories;
};

/// Where a path printed on a row may be, first to last (§4.4 of the link-detection design): a relative path
/// in each of @p bases, `~/` in @p home, an absolute path as it is. Lexically normalised.
/// An absolute and a `~/` path do not depend on @p bases, so they are placed even when no base is known.
/// `..` and `.` are collapsed lexically (§4.4), without asking the filesystem: `/a/link/../x` means `/a/x`
/// even where `link` is a symlink to elsewhere, which the kernel would resolve physically. Only a leading
/// `~/` is expanded: `~user/...` and a bare `~` are not.
/// @param bases The directories of the command that printed the row, first to last (PathBases).
/// @param home The home directory; empty when unknown.
/// @param printed The path as printed.
/// @return The candidates and the base each was joined to; none when the path cannot be placed (relative
///         with no base, `~/` with no home).
[[nodiscard]] PathCandidates pathCandidates(std::span<std::string const> bases,
                                            std::string_view home,
                                            std::string_view printed);

/// An existing local file or directory.
struct ResolvedPath
{
    std::filesystem::path
        path; ///< Lexically normalised; absolute only if the base or home it was joined to was.
    PathKind kind = PathKind::Missing; ///< File or Directory.
    /// Which of the candidates it was: an index into PathCandidates::paths, so PathCandidates::directories
    /// at it names the base a relative path was found in. Not an index into the bases.
    std::size_t candidateIndex = 0;
};

/// The first of @p candidates that names a regular file or a directory. Blocks on the filesystem.
/// @param fileSystem Where to look.
/// @param candidates What pathCandidates() placed.
/// @return The first existing one; nullopt when none exists.
[[nodiscard]] std::optional<ResolvedPath> resolveLocalPath(core::platform::FileSystem const& fileSystem,
                                                           std::span<std::filesystem::path const> candidates);

/// @p printed, pointing at what @p answer found: its place, what it names, and the directory a relative path
/// was found in. The one way a path link is completed, by detection, by the follow gesture and by its probe.
/// @param printed A path link, its target as printed.
/// @param answer What resolveLocalPath() or a probe found for the query made of @p printed; not Missing.
/// @param directories PathCandidates::directories of that query: one per candidate.
/// @return The link, resolved.
[[nodiscard]] LinkTarget resolvedPathTarget(LinkTarget printed,
                                            ResolvedPath const& answer,
                                            std::span<std::string const> directories);

} // namespace vtbackend
