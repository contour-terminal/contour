// SPDX-License-Identifier: Apache-2.0
//
// Which root a directory belongs to, and the directories a declared command may run in or name: a working
// directory is opened by handle and judged by the path read back from it, a `path:<root>` argument by its
// longest existing ancestor. A directory belongs to its most specific root, so that a root nested inside
// another keeps the commands of the outer one out.
//
// A path a guest supplies is judged lexically, then opened, then judged again by handle. Opening is not
// harmless: on Windows a UNC path is a connection to its server, which is handed the user's credentials, and
// on POSIX an automounter or a FUSE mount answers a lookup. So nothing is opened unless the path as written
// is at or beneath one of its root's directories (RootPathSyntax's isAtOrBeneath()); what the open then
// resolves to (a symlink, a junction, an 8.3 name, another case) is judged again by the path read back from
// the handle.
#pragma once

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/PathStyle.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <vtextension/DaemonWorld.hpp>

namespace vtextension
{

/// Why a directory, or a path, is not one a root's command may use.
enum class RootError : uint8_t
{
    NotAbsolute,      ///< The path is not absolute in the opener's grammar.
    Unnormalized,     ///< The path has an empty, `.` or `..` component, or is too long.
    Missing,          ///< Nothing exists at the path, or the root's directory could not be created.
    OutsideRoots,     ///< The resolved path lies inside no declared root.
    NotItsRoot,       ///< The resolved path lies inside a root, but its most specific root is another one.
    EqualRoots,       ///< Two declared roots resolve to the same directory.
    CreateInsideRoot, ///< A missing `create` root lies inside another declared root, so it is not created.
    Unopenable,       ///< Something exists at the path, but it is no directory, or may not be opened.
    Unsupported,      ///< This platform has no directory handles.
    Unresolved,       ///< A root a declaration names has no resolved entry.
};

/// Why a working directory is refused, whichever service was asked to run something in it: the one
/// vocabulary declared processes and agent panes share, so that the same directory is refused in the same
/// words by both.
enum class CwdRefusal : uint8_t
{
    OutsideRoot,   ///< It is not a directory of the root the declaration names.
    Missing,       ///< It does not exist, or its root could not be created.
    NotADirectory, ///< Something exists there, but no directory, or none that may be opened.
    Unsupported,   ///< This platform has no directory handles.
};

/// How a refused working directory is told, indexed by RootError.
inline constexpr auto CwdRefusalOfRootError = std::array {
    CwdRefusal::OutsideRoot,   // NotAbsolute
    CwdRefusal::OutsideRoot,   // Unnormalized
    CwdRefusal::Missing,       // Missing
    CwdRefusal::OutsideRoot,   // OutsideRoots
    CwdRefusal::OutsideRoot,   // NotItsRoot
    CwdRefusal::OutsideRoot,   // EqualRoots
    CwdRefusal::Missing,       // CreateInsideRoot
    CwdRefusal::NotADirectory, // Unopenable
    CwdRefusal::Unsupported,   // Unsupported
    CwdRefusal::OutsideRoot,   // Unresolved
};
static_assert(CwdRefusalOfRootError.size() == std::to_underlying(RootError::Unresolved) + 1U,
              "every RootError has a row");

/// @param error Why the working directory could not be used.
/// @return How the refusal is told, by CwdRefusalOfRootError.
[[nodiscard]] constexpr CwdRefusal cwdRefusalOf(RootError error) noexcept
{
    return CwdRefusalOfRootError.at(std::to_underlying(error));
}

/// Finds the root a resolved path belongs to.
/// @param roots The declared roots, resolved.
/// @param resolvedPath An absolute, normalised path, symlinks resolved.
/// @param style The grammar the roots and @p resolvedPath are spelled in.
/// @return The label of the most specific root holding @p resolvedPath (component-wise prefix by
///         RootPathSyntax's isAtOrBeneath(), the root's own directory included); std::nullopt when none holds
///         it.
[[nodiscard]] std::optional<std::string_view> mostSpecificRoot(std::span<ResolvedRoot const> roots,
                                                               std::string_view resolvedPath,
                                                               vtpty::PathStyle style);

/// Creates a missing `create` root, unless its path lies inside another declared root; an existing one is
/// used wherever it lies. The missing directories are created one component at a time beneath a handle on
/// the longest existing ancestor, following no link, and judged by that handle's resolved path.
/// @param opener Where directories are opened and created.
/// @param roots The declared roots, resolved.
/// @param root The label of the root to make sure of.
/// @return Nothing once every directory of @p root exists; `CreateInsideRoot` for a missing one inside
/// another
///         root, `Unresolved` for a label no root has; why a directory could not be created otherwise.
[[nodiscard]] std::expected<void, RootError> ensureRoot(vtpty::IDirectoryOpener& opener,
                                                        std::span<ResolvedRoot const> roots,
                                                        std::string_view root);

/// Opens an existing directory and judges it by the path read back from its handle: inside @p root, and
/// @p root its most specific root. A path not at or beneath one of @p root's directories as written is
/// refused without being opened.
/// @param opener Where directories are opened.
/// @param roots The declared roots, resolved.
/// @param root The label of the root the directory must belong to.
/// @param path The directory; absolute and normalised.
/// @return The open directory; or why it may not be used.
[[nodiscard]] std::expected<vtpty::DirectoryHandle, RootError> openInRoot(vtpty::IDirectoryOpener& opener,
                                                                          std::span<ResolvedRoot const> roots,
                                                                          std::string_view root,
                                                                          std::string_view path);

/// Resolves a `path:<root>` argument, which may not exist yet, creating nothing: its longest existing
/// ancestor opened, the rest appended, and the result judged as openInRoot() judges a directory (as written
/// first, then as resolved), and strictly below @p root's own directory.
/// @param opener Where paths are opened.
/// @param roots The declared roots, resolved.
/// @param root The label of the root the argument is declared inside.
/// @param value The argument's value; absolute and normalised.
/// @return The resolved path; or why the value is refused.
[[nodiscard]] std::expected<std::string, RootError> resolvePathArgument(vtpty::IDirectoryOpener& opener,
                                                                        std::span<ResolvedRoot const> roots,
                                                                        std::string_view root,
                                                                        std::string_view value);

/// Creates the missing directories above the last component of a `path:<root>` argument, as ensureRoot()
/// creates them: resolved and judged again as resolvePathArgument() does, then one component at a time
/// beneath a handle on the longest existing ancestor, following no link. Only the last component is left to a
/// name: the residual docs/extensions.md describes. Called once nothing else refuses the start.
/// @param opener Where paths are opened and directories created.
/// @param roots The declared roots, resolved.
/// @param root The label of the root the argument is declared inside.
/// @param value The argument's value; absolute and normalised.
/// @return The resolved path, which is what the command is given; or why the value is refused or a directory
///         could not be created.
[[nodiscard]] std::expected<std::string, RootError> createPathParents(vtpty::IDirectoryOpener& opener,
                                                                      std::span<ResolvedRoot const> roots,
                                                                      std::string_view root,
                                                                      std::string_view value);

/// Checks that no two declared directories are the same, which would leave a directory with two most
/// specific roots.
/// @param roots The declared roots, resolved.
/// @param style The grammar the roots are spelled in.
/// @return Nothing; `EqualRoots` when two paths are equal by samePath()'s rule (Windows: ASCII case folded).
[[nodiscard]] std::expected<void, RootError> checkRootsDistinct(std::span<ResolvedRoot const> roots,
                                                                vtpty::PathStyle style);

} // namespace vtextension
