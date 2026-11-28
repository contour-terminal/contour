// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One daemon as a window's sidebar lists it: which daemon it is, where its link stands, and the workspaces
/// it serves. Qt-free and free of `remote/`, so the sidebar's model and its tests speak it without a daemon.

#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonLinkLook.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <compare>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace contour::window
{

/// Names one daemon across this process, stable for as long as it is configured: what the GUI state keys a
/// daemon's listing choice by. A local daemon and a saved host of the same name are two daemons.
struct DaemonRef
{
    std::string value; ///< `local:<label>`, `host:<name>` or `link:<id>`.

    auto operator<=>(DaemonRef const&) const = default;
};

/// What kind of daemon a @c DaemonRef names.
enum class DaemonRefKind : uint8_t
{
    Local = 0, ///< A daemon on this machine, by its socket label.
    Host,      ///< A saved host, by its name in the configuration.
    Link,      ///< A daemon reached by an ad-hoc link that has no saved name, by the link's id.
};

/// The prefix that spells one @c DaemonRefKind in a @c DaemonRef.
struct DaemonRefPrefix
{
    DaemonRefKind kind;      ///< The kind.
    std::string_view prefix; ///< Its spelling, colon included.
};

/// The prefixes, indexed by @c DaemonRefKind.
inline constexpr auto DaemonRefPrefixes = std::array {
    DaemonRefPrefix { DaemonRefKind::Local, "local:" },
    DaemonRefPrefix { DaemonRefKind::Host, "host:" },
    DaemonRefPrefix { DaemonRefKind::Link, "link:" },
};

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, DaemonRefPrefixes.size()),
                                  [](size_t i) { return DaemonRefPrefixes[i].kind == DaemonRefKind(i); }),
              "DaemonRefPrefixes must list the kinds in enumerator order");

/// @param kind The kind of daemon.
/// @param name What names it within its kind.
/// @return The reference spelled as `<prefix><name>`.
[[nodiscard]] inline DaemonRef daemonRefOf(DaemonRefKind kind, std::string_view name)
{
    return DaemonRef { std::format("{}{}", DaemonRefPrefixes[static_cast<size_t>(kind)].prefix, name) };
}

/// @param label The local daemon's socket label.
/// @return `local:<label>`.
[[nodiscard]] inline DaemonRef localDaemonRef(std::string_view label)
{
    return daemonRefOf(DaemonRefKind::Local, label);
}

/// @param host The saved host's name.
/// @return `host:<name>`.
[[nodiscard]] inline DaemonRef hostDaemonRef(std::string_view host)
{
    return daemonRefOf(DaemonRefKind::Host, host);
}

/// @param link The id of the link that reached the daemon.
/// @return `link:<id>`.
[[nodiscard]] inline DaemonRef linkDaemonRef(uint32_t link)
{
    return daemonRefOf(DaemonRefKind::Link, std::to_string(link));
}

/// Reads a reference back into its kind and its name.
/// @param ref The reference.
/// @return The kind and the name (a view into @p ref); nullopt for an unknown prefix, an empty name, or a
///         link whose id is not a number.
[[nodiscard]] inline std::optional<std::pair<DaemonRefKind, std::string_view>> parseDaemonRef(
    DaemonRef const& ref)
{
    auto const text = std::string_view { ref.value };
    auto const prefix = std::ranges::find_if(DaemonRefPrefixes, [&](DaemonRefPrefix const& candidate) {
        return text.starts_with(candidate.prefix);
    });
    if (prefix == DaemonRefPrefixes.end())
        return std::nullopt;
    auto const name = text.substr(prefix->prefix.size());
    if (name.empty())
        return std::nullopt;
    if (prefix->kind == DaemonRefKind::Link)
    {
        auto id = uint32_t { 0 };
        auto const [end, error] = std::from_chars(name.data(), name.data() + name.size(), id);
        if (error != std::errc {} || end != name.data() + name.size())
            return std::nullopt;
    }
    return std::pair { prefix->kind, name };
}

/// One run of a daemon: a daemon that restarts is the same @c DaemonRef under a new instance, so that what
/// was keyed by the old run (folds, panes) is not mistaken for the new run's.
struct DaemonInstance
{
    uint64_t value = 0; ///< The run's identity; 0 before the daemon has said.

    auto operator<=>(DaemonInstance const&) const = default;
};

/// A daemon the user chose not to list, as the Hidden row names it.
struct HiddenDaemon
{
    DaemonRef daemon; ///< Which daemon.
    std::string name; ///< What it is called.

    bool operator==(HiddenDaemon const&) const = default;
};

/// Whether the user chose to list a daemon in the sidebar.
enum class SidebarListingChoice : uint8_t
{
    Unlisted = 0, ///< Not listed.
    Listed,       ///< Listed.
};

/// Where a section's facts come from.
enum class SectionFeed : uint8_t
{
    /// A window's link to the daemon, or a window's switch holding its connection on the way in or out: that
    /// window acts on it.
    WindowLink = 0,
    Observer, ///< An observer link this process keeps to a daemon no window of it is on.
};

/// One daemon as a window's sidebar lists it.
struct DaemonSection
{
    DaemonRef daemon;                                      ///< Which daemon.
    DaemonInstance instance;                               ///< Which run of it.
    std::string name;                                      ///< What the row calls it.
    DaemonLocality locality = DaemonLocality::Remote;      ///< Whether it runs on this machine.
    DaemonLinkState state = DaemonLinkState::NotConnected; ///< Where the link stands.
    std::string reason;                                    ///< Why it failed; empty otherwise.
    WorkspaceDirectory directory;                          ///< Its workspaces, last known.
    /// Every workspace any window of this process shows; a window subtracts its own.
    std::set<uint64_t> shownElsewhere;
    SectionFeed feed = SectionFeed::WindowLink; ///< Where these facts come from.

    bool operator==(DaemonSection const&) const = default;
};

} // namespace contour::window
