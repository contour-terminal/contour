// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/TerminalContext.hpp>

#include <cstdint>
#include <optional>
#include <string>

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
/// @param osc7Url   The OSC 7 working-directory URL, or empty when none was reported.
/// @param self      This machine's identity, injected. @see LocalIdentity.
/// @param purpose   What the answer is wanted for.
/// @return The resolved directory, or nullopt when no source could answer for this purpose.
[[nodiscard]] std::optional<ResolvedWorkingDirectory> resolveWorkingDirectory(ContextStack const& contexts,
                                                                              std::string const& osc7Url,
                                                                              LocalIdentity const& self,
                                                                              CwdPurpose purpose);

} // namespace vtbackend
