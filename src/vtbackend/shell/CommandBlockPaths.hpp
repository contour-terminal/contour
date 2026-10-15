// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace vtbackend
{

/// Why no path printed on a row names a file on this machine.
enum class PathRefusal : uint8_t
{
    RanElsewhere = 0,        ///< The command that printed it ran on another machine: ssh, a container.
    ReplicaOfAnotherMachine, ///< The terminal replicates one on another machine, whose files its paths name.
};

/// Whether @p block recorded a directory provably on this machine, as CwdPurpose::OpenLocally demands one:
/// an unvouched directory (ContextLocality::Unknown, an OSC 3008 `cwd=` no identity backs) may name one on
/// the far side of an ssh.
/// @param block A block's record.
/// @return Whether a relative path its command printed resolves against its own directory.
[[nodiscard]] bool ranInLocalDirectory(CommandBlockRecord const& block) noexcept;

/// What the terminal reports about its own working directory, as localPathBaseFor() weighs it.
struct TerminalDirectory
{
    /// The directory for opening locally (CwdPurpose::OpenLocally); empty when there is none.
    std::string openLocally;
    /// Where the directory shown to the user (CwdPurpose::Display) is: Foreign once whatever runs in the
    /// terminal reported one on another machine.
    ContextLocality locality = ContextLocality::Unknown;
};

/// The directories a relative path printed on a row is looked up in, first to last: the first that holds the
/// name wins. None when there is nowhere to look, and then only an absolute or home-relative path resolves.
using PathBases = std::vector<std::string>;

/// The directories a relative path printed on a row resolves against.
///
/// The directory the command that printed it ran in, not the shell's directory now: `cd` between two
/// commands must not re-point the paths an earlier one printed. Where that directory is not provably local
/// (ranInLocalDirectory()) the terminal's stands in, as resolveWorkingDirectory() passes over an unvouched
/// one -- unless the command ran on another machine. Then the directory the command left the shell in, for
/// a name it printed after a `cd` in its own command line: `cd build && make` prints paths relative to
/// `build`, which the shell reports only at the next prompt.
///
/// A block not yet finished is still printing, so the terminal's directory now speaks for where: once that
/// turned foreign -- `ssh host` reporting the remote's directory without marking its commands -- the rest
/// of the block came from there. The block records that it did (CommandBlockRecord::foreignDirectory), so
/// the local prompt that finishes it leaves it refused.
/// @param block The record of the block the row belongs to; nullptr for a row outside any block.
/// @param terminal The terminal's own working directory.
/// @return The block's directory when it ran in a local one, else the terminal's for opening locally, then
///         the local one its command left the shell in; outside any block, the terminal's alone. Either may
///         be missing. RanElsewhere when the command ran on another machine (ssh, a container), whose paths
///         -- absolute ones included -- name nothing here.
[[nodiscard]] std::expected<PathBases, PathRefusal> localPathBaseFor(CommandBlockRecord const* block,
                                                                     TerminalDirectory const& terminal);

} // namespace vtbackend
