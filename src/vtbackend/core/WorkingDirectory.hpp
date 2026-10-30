// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/core/TerminalContext.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

/// What a caller wants a working directory FOR.
///
/// The three questions have three different right answers, and conflating them is what makes a
/// container's `/app` end up handed to a host fork+exec:
///
///   - "Where should a new tab on this machine start?"  -> a path this process can chdir into.
///   - "What should I show the user?"                   -> where they believe they are, boundary or not.
///   - "Open this folder in the host file manager?"     -> refuse unless it is provably local.
///
/// A table rather than three near-copies of the precedence, so a fourth question is a row.
enum class CwdPurpose : uint8_t
{
    /// Spawning a child on THIS machine. Refuses anything not provably local.
    Spawn = 0,

    /// Showing the user where they are. A container or remote path is the RIGHT answer here -- it is
    /// what the user is looking at -- so no locality filter applies.
    Display,

    /// Opening the path with a host tool (a file manager). Refuses on the same terms as Spawn; the
    /// caller greys the affordance out rather than opening the host's directory of the same name.
    OpenLocally,
};

/// Whether @p purpose will actually open or chdir into the path, and therefore needs it to be on this
/// machine.
/// @param purpose What a working directory is wanted for.
/// @return Whether only a provably local path will do.
[[nodiscard]] constexpr bool needsLocalPath(CwdPurpose purpose) noexcept
{
    return purpose != CwdPurpose::Display;
}

/// Whether a directory of @p locality may serve @p purpose: anywhere for showing it, only on this machine
/// for whatever needsLocalPath().
/// @param purpose What a working directory is wanted for.
/// @param locality Whether the directory is on this machine.
/// @return Whether a directory there answers @p purpose.
[[nodiscard]] constexpr bool usableFor(CwdPurpose purpose, ContextLocality locality) noexcept
{
    return !needsLocalPath(purpose) || locality == ContextLocality::Local;
}

/// Where a resolved working directory came from.
enum class CwdSource : uint8_t
{
    None = 0,      ///< Nothing had anything to say.
    ContextSignal, ///< The nearest `cwd=` on the OSC 3008 ancestry.
    Osc7,          ///< The OSC 7 working-directory URL.
};

/// A working directory, where it came from, and whether it is this machine's.
struct ResolvedWorkingDirectory
{
    std::string path;
    CwdSource source = CwdSource::None;
    ContextLocality locality = ContextLocality::Unknown;

    bool operator==(ResolvedWorkingDirectory const&) const = default;
};

/// Resolves the working directory for @p purpose from the two sources a terminal has.
///
/// The order is the same for every purpose -- the OSC 3008 ancestry first, then OSC 7 -- and only the
/// FILTER differs. That is deliberate: "where does a new tab start" and "what does the tooltip say"
/// must never disagree about precedence, only about how strict they are.
///
/// Note what this deliberately does NOT do: it never claims a path is local without evidence. An OSC
/// 3008 `cwd=` with no boundary, no machineid and no hostname resolves Unknown, and Spawn/OpenLocally
/// reject it -- because nothing emits a `remote` context for ssh today, so a remote shell's sequences
/// look entirely local and its `/home/bob` may well exist here too. The caller's own fallback (on
/// POSIX, the pty child's /proc cwd) is a better answer than a guess.
///
/// @param contexts  The OSC 3008 ancestry.
/// @param osc7      The OSC 7 working-directory URL, parsed; its url is empty when none was reported.
/// @param self      This machine's identity, injected. @see LocalIdentity.
/// @param purpose   What the answer is wanted for.
/// @return The resolved directory, or nullopt when no source could answer for this purpose.
[[nodiscard]] std::optional<ResolvedWorkingDirectory> resolveWorkingDirectory(ContextStack const& contexts,
                                                                              FileUrl const& osc7,
                                                                              LocalIdentity const& self,
                                                                              CwdPurpose purpose);

/// Replaces a leading home directory with `~`, the way a shell prompt writes a path.
///
/// Purely presentational -- the tab tooltip, a finish notification: a caller
/// that wants a path to ACT on wants the real one, which is why this is never done at the source.
///
/// Only a whole path component matches, so a sibling directory whose name merely starts with the home
/// path (`/home/bobby` against a home of `/home/bob`) is left alone rather than mangled into `~by`.
///
/// @param path The absolute path to abbreviate.
/// @param home The user's home directory, with or without a trailing separator. An empty value
///             abbreviates nothing.
/// @return @p path with the home prefix replaced by `~`, or @p path unchanged.
[[nodiscard]] std::string abbreviateHomePath(std::string_view path, std::string_view home);

/// The path @p directory reads as where it is SHOWN -- the pane header, the status line's `{Cwd}`:
/// abbreviated as a shell prompt writes it, but only for a directory on this machine, because this
/// machine's home says nothing about another machine's paths (an ssh shell's `/home/bob` is not `~`).
///
/// One function for every such surface, so two views of the same pane cannot disagree about it. Not made
/// safe to show: the caller sanitizes, as it does any text it draws.
///
/// @param directory The resolved working directory.
/// @param home The user's home directory. An empty value abbreviates nothing.
/// @return The path, home-abbreviated when @p directory is Local.
[[nodiscard]] std::string homeAbbreviatedPath(ResolvedWorkingDirectory const& directory,
                                              std::string_view home);

/// The path @p path reads as where it is shown, by the rule of the overload above, for a directory known
/// only by its path and locality -- a command block's snapshot, as a finish notification shows it.
///
/// @param path The absolute path.
/// @param locality Whether @p path names this machine; only a Local one is abbreviated.
/// @param home The user's home directory. An empty value abbreviates nothing.
/// @return The path, home-abbreviated when @p locality is Local.
[[nodiscard]] std::string homeAbbreviatedPath(std::string_view path,
                                              ContextLocality locality,
                                              std::string_view home);

} // namespace vtbackend
