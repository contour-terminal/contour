// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A profile catalog over a map, for the tests of what a profile name resolves to.

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <vthost/ProfileCatalog.hpp>

namespace vthost::testing
{

/// Resolves names in a fixed map; an empty or unknown name resolves to @ref defaultProfile.
class FakeProfileCatalog final: public ProfileCatalog
{
  public:
    /// @param profiles The profiles, by name; each entry's own name and match are filled in on resolve.
    /// @param defaultProfile The name an empty or unknown one resolves to; must be in @p profiles.
    FakeProfileCatalog(std::map<std::string, ResolvedProfile> profiles, std::string defaultProfile):
        _profiles { std::move(profiles) }, _defaultProfile { std::move(defaultProfile) }
    {
    }

    [[nodiscard]] ResolvedProfile resolve(std::string_view name) const override
    {
        auto const named = _profiles.find(std::string { name });
        auto const found = named != _profiles.end() && !name.empty();
        auto resolved = found ? named->second : _profiles.at(_defaultProfile);
        resolved.name = found ? std::string { name } : _defaultProfile;
        resolved.match = found ? ProfileMatch::Named : ProfileMatch::Default;
        return resolved;
    }

  private:
    std::map<std::string, ResolvedProfile> _profiles;
    std::string _defaultProfile;
};

/// @param program The shell the profile runs.
/// @param history The scrollback its sessions keep.
/// @return A profile running @p program with @p history lines of scrollback.
[[nodiscard]] inline ResolvedProfile fakeProfile(std::string program, vtbackend::LineCount history)
{
    auto profile = ResolvedProfile {};
    profile.shell.program = std::move(program);
    profile.settings.historyLimits = vtbackend::HistoryLimits::plain(history);
    return profile;
}

} // namespace vthost::testing
