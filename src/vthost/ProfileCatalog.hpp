// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The host's profiles, by name: what a session a client names a profile for runs. The host's own
/// configuration decides it — the shell, its environment and directory, the scrollback — because that is
/// what runs on the host; a client only names the profile.

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/screen/Settings.hpp>

#include <vtpty/Process.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace vthost
{

/// Whether the name asked for was found, or the default profile stands in for it.
enum class ProfileMatch : uint8_t
{
    Default = 0, ///< The name was empty or unknown here: the host's default profile.
    Named = 1,   ///< The host has a profile of that name.
};

/// Whether a profile's sessions escape the sandbox the daemon runs in (Flatpak), as its `escape_sandbox`
/// says.
enum class SandboxEscape : uint8_t
{
    Contained = 0, ///< The shell runs inside the daemon's sandbox (`escape_sandbox: false`).
    Escapes = 1,   ///< The shell runs on the host (`escape_sandbox: true`, the default).
};

/// The light/dark preference a host resolves a session's palette under until a client states its own
/// (proto::EngineOperation::UseDarkColors / UseLightColors): a daemon has no display to ask.
inline constexpr auto HostedColorPreference = vtbackend::ColorPreference::Light;

/// A profile as the host resolved it.
struct ResolvedProfile
{
    std::string name;               ///< The profile resolved: the one asked for, or the default's name.
    vtbackend::Settings settings;   ///< The emulation and presentation its sessions are created with.
    vtpty::Process::ExecInfo shell; ///< What its sessions run: program, arguments, directory, environment.
    ProfileMatch match = ProfileMatch::Default; ///< Whether @ref name is the one asked for.
    /// Whether its shell escapes the sandbox: the profile's own, never the default profile's.
    SandboxEscape sandbox = SandboxEscape::Escapes;
};

/// Resolves a profile name in the host's own configuration.
class ProfileCatalog
{
  public:
    ProfileCatalog() = default;
    ProfileCatalog(ProfileCatalog const&) = delete;
    ProfileCatalog(ProfileCatalog&&) = delete;
    ProfileCatalog& operator=(ProfileCatalog const&) = delete;
    ProfileCatalog& operator=(ProfileCatalog&&) = delete;
    virtual ~ProfileCatalog() = default;

    /// Never fails: a name empty or unknown here resolves to the default profile, with
    /// ProfileMatch::Default — a client is never refused for naming a profile the host lacks.
    /// @param name The profile a client named.
    /// @return The profile.
    [[nodiscard]] virtual ResolvedProfile resolve(std::string_view name) const = 0;

    /// The palette a profile's sessions use under a light/dark preference: its dark scheme or its light one
    /// over a dual color configuration, its single scheme otherwise.
    ///
    /// The default answers with whatever @ref resolve gives, which is right for a catalog whose profiles hold
    /// one palette each.
    /// @param name The profile a client named; empty or unknown resolves to the default profile.
    /// @param preference The preference to resolve the palette under.
    /// @return The palette.
    [[nodiscard]] virtual vtbackend::ColorPalette colorPalette(
        std::string_view name, vtbackend::ColorPreference /*preference*/) const
    {
        return resolve(name).settings.colorPalette;
    }
};

} // namespace vthost
