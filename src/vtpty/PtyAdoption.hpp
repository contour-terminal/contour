// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a live upgrade's adopter does with a PTY it inherited, whatever the platform: adopts it, or closes
/// what it names; and lets go of a child it did not adopt. The other half of Pty::handover().

#include <vtpty/InheritedPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <expected>
#include <memory>

namespace vtpty
{

/// Takes over the PTY a previous image, or the daemon a Windows successor takes over from, left behind: the
/// platform's own adopt, UnixPty::adopt() on POSIX and ConPty::adopt() on Windows.
/// @param pty What Pty::handover() named, as it crossed.
/// @return The running PTY; or why it could not be adopted, nothing of it then closed (@see
/// closeInherited()).
[[nodiscard]] std::expected<std::unique_ptr<Pty>, AdoptError> adoptPty(InheritedPty const& pty);

/// Asks whether adoptPty() would take @p pty, without taking anything: what a Windows successor checks before
/// it says it can take over. The platform's own check, UnixPty::canAdopt() on POSIX and ConPty::canAdopt() on
/// Windows.
/// @param pty What Pty::handover() named, as it crossed.
/// @return Nothing; or why adoptPty() would refuse it.
[[nodiscard]] std::expected<void, AdoptError> canAdoptPty(InheritedPty const& pty);

/// Asks whether this process can adopt any pseudoconsole at all, without taking anything: what a release
/// reports of itself, for the daemon that would hand its pseudoconsoles over to ask first. On Windows the
/// bundled conpty.dll's own check (ConPty::canAdoptAny()), which loads it.
/// @return NotApplicable on POSIX; Able or Unable on Windows.
[[nodiscard]] PseudoconsoleAdoptability pseudoconsoleAdoptability();

/// Closes every handle or descriptor @p pty names that is not none: what a PTY that could not be adopted
/// leaves, which would otherwise stay open, unused -- while the program on it, never seeing a hangup, waited
/// for input forever. On POSIX the standard streams are never a PTY's, and are left alone whatever @p pty
/// names.
/// @param pty What Pty::handover() named, as it crossed.
void closeInherited(InheritedPty const& pty) noexcept;

/// Lets go of what this process holds of a child it did not adopt: on Windows the handle to it, closed; on
/// POSIX nothing, the child being waited for by its process id, by whoever reaps it.
/// @param child The child, as it crossed.
void releaseUnadopted(AdoptedChild const& child) noexcept;

} // namespace vtpty
