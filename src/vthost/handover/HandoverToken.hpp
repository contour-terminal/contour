// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The argument that names a live upgrade's handover to the daemon that takes over, `--handover=TOKEN`: its
/// one grammar, formatted and parsed here alone. TOKEN is `fd=N` on POSIX, the descriptor of the memory
/// object the image is in (ExecHandoff), and `handles=M,R,C,P` on Windows, the handles a successor process
/// is handed (SuccessorHandoff). Compiled on every platform, so that either form is tested anywhere.

#include <vtpty/CrossingHandle.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace vthost::handover
{

/// What names a handover to the daemon that takes over: `--handover=TOKEN`. A command line names one: the
/// one it carried is dropped before the next upgrade appends its own, so that a daemon names only the image
/// it was handed, however many it has seen.
inline constexpr auto HandoverPrefix = std::string_view { "--handover=" };

/// The handles a successor process is handed beside those the image names: what `--handover=handles=` names.
struct SuccessorHandles
{
    vtpty::CrossingHandle image {}; ///< The unnamed mapping the image is in (HandoverMemory).
    /// The event the successor sets once it has read the image and can take over.
    vtpty::CrossingHandle ready {};
    /// The event the predecessor sets once it has seen the successor ready: the successor owns everything
    /// from then on.
    vtpty::CrossingHandle commit {};
    /// The predecessor process, which the successor waits to have ended.
    vtpty::CrossingHandle predecessor {};

    bool operator==(SuccessorHandles const&) const = default;
};

/// @param memory The memory object a POSIX image is in.
/// @return The token that names it, `fd=N`: what follows `--handover=` (HandoverPrefix).
[[nodiscard]] std::string handoverToken(vtpty::CrossingHandle memory);

/// @param handles What a Windows successor is handed.
/// @return The token that names them, `handles=M,R,C,P`: what follows `--handover=` (HandoverPrefix).
[[nodiscard]] std::string handoverToken(SuccessorHandles const& handles);

/// @param token The token to name.
/// @return The whole argument, `--handover=TOKEN`.
[[nodiscard]] std::string handoverArgument(std::string_view token);

/// @param argument One argument of a command line.
/// @return What follows `--handover=` in it; nullopt for an argument that names no handover.
[[nodiscard]] std::optional<std::string_view> handoverTokenOf(std::string_view argument) noexcept;

/// @param token What follows `--handover=`.
/// @return The descriptor it names; nullopt for a token that is not `fd=` and one descriptor, or that names a
///         standard stream (0 to 2) or a value no descriptor can have (above INT_MAX).
[[nodiscard]] std::optional<vtpty::CrossingHandle> parseHandoverDescriptor(std::string_view token) noexcept;

/// @param token What follows `--handover=`.
/// @return The handles it names; nullopt for a token that is not `handles=` and four handles, each of them
///         positive.
[[nodiscard]] std::optional<SuccessorHandles> parseHandoverHandles(std::string_view token) noexcept;

} // namespace vthost::handover
