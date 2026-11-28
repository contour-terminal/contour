// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/PresencePlan.hpp>
#include <contour/remote/PresenceTransport.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <functional>
#include <ranges>
#include <string_view>

namespace contour::remote
{

namespace
{
    using vthost::client::ConnectionState;
    using window::DaemonLinkState;

    /// Where a connection state's link state comes from.
    enum class LinkStateSource : uint8_t
    {
        Connection = 0, ///< The row's own state.
        LastFailure,    ///< The connection's last transport failure; Failed without one that says more.
    };

    /// What a connection state says of the link.
    struct ConnectionRow
    {
        ConnectionState connection;
        DaemonLinkState state;
        LinkStateSource source;
    };

    constexpr auto ConnectionRows = std::to_array<ConnectionRow>({
        { .connection = ConnectionState::Connecting,
          .state = DaemonLinkState::Connecting,
          .source = LinkStateSource::Connection },
        { .connection = ConnectionState::Connected,
          .state = DaemonLinkState::Connected,
          .source = LinkStateSource::Connection },
        { .connection = ConnectionState::Reconnecting,
          .state = DaemonLinkState::Reconnecting,
          .source = LinkStateSource::Connection },
        { .connection = ConnectionState::Failed,
          .state = DaemonLinkState::Failed,
          .source = LinkStateSource::LastFailure },
        { .connection = ConnectionState::Refused,
          .state = DaemonLinkState::Failed,
          .source = LinkStateSource::LastFailure },
        { .connection = ConnectionState::Stopped,
          .state = DaemonLinkState::NotConnected,
          .source = LinkStateSource::Connection },
        // The daemon replaces itself in place: the same daemon is waited for.
        { .connection = ConnectionState::Upgrading,
          .state = DaemonLinkState::Reconnecting,
          .source = LinkStateSource::Connection },
        { .connection = ConnectionState::NeedsAttention,
          .state = DaemonLinkState::NeedsInput,
          .source = LinkStateSource::Connection },
    });

    static_assert(ConnectionRows.size() == vthost::client::ConnectionStateCount,
                  "ConnectionRows needs a row for every ConnectionState");
    static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, ConnectionRows.size()),
                                      [](size_t i) {
                                          return ConnectionRows[i].connection == ConnectionState(i);
                                      }),
                  "ConnectionRows must list the states in enumerator order");

    /// The label every local daemon is started under unless another is named; listed first.
    constexpr auto DefaultLocalLabel = std::string_view { "default" };

    /// @return The numeric id of a `link:` ref's name; 0 for one that does not parse (none is listed).
    [[nodiscard]] uint32_t linkIdOf(std::string_view name)
    {
        auto id = uint32_t { 0 };
        std::from_chars(name.data(), name.data() + name.size(), id);
        return id;
    }

    /// Collects the listed daemons, each once, in the order they are added.
    class Listing
    {
      public:
        /// Lists @p daemon, unless it is listed already.
        void add(window::DaemonRef daemon, window::DaemonLocality locality)
        {
            if (!_added.insert(daemon.value).second)
                return;
            _listed.push_back(ListedDaemon { .daemon = std::move(daemon), .locality = locality });
        }

        /// @return The listed daemons, in the order they were added.
        [[nodiscard]] std::vector<ListedDaemon> take() { return std::move(_listed); }

      private:
        std::set<std::string> _added;
        std::vector<ListedDaemon> _listed;
    };

    /// @param inputs What discovery reports.
    /// @param labels Labels known besides: the local daemons a window is on.
    /// @return The labels of the local daemons known this run -- running now, listed earlier, or in
    ///         @p labels -- the default daemon first, the rest alphabetically.
    [[nodiscard]] std::vector<std::string> knownLocalLabels(PresenceInputs const& inputs,
                                                            std::set<std::string> labels)
    {
        labels.insert(inputs.runningLocal.begin(), inputs.runningLocal.end());
        labels.insert(inputs.seenLocalThisRun.begin(), inputs.seenLocalThisRun.end());
        // std::set holds the labels alphabetically; the default daemon goes first.
        auto ordered = std::vector<std::string>(labels.begin(), labels.end());
        std::ranges::stable_partition(ordered,
                                      [](std::string const& label) { return label == DefaultLocalLabel; });
        return ordered;
    }

    /// @return The saved hosts, by name.
    [[nodiscard]] std::vector<std::pair<std::string, window::SidebarListingChoice>> hostsByName(
        ListingSettings const& settings)
    {
        auto hosts = settings.hosts;
        std::ranges::stable_sort(
            hosts, std::less {}, [](auto const& entry) -> std::string const& { return entry.first; });
        return hosts;
    }
} // namespace

std::vector<ListedDaemon> listedDaemons(PresenceInputs const& inputs)
{
    if (inputs.windowsOn.empty())
        return {};

    auto const& settings = inputs.settings;
    auto windowOn = std::set<std::string> {};
    auto localLabels = std::set<std::string> {};
    auto unconfiguredHosts = std::set<std::string> {};
    auto links = std::vector<std::pair<uint32_t, window::DaemonRef>> {};
    auto const configured = [&](std::string_view host) {
        return std::ranges::contains(
            settings.hosts, host, [](auto const& entry) -> std::string_view { return entry.first; });
    };
    for (auto const& daemon: inputs.windowsOn)
    {
        auto const parsed = window::parseDaemonRef(daemon);
        if (!parsed)
            continue;
        windowOn.insert(daemon.value);
        auto const& [kind, name] = *parsed;
        switch (kind)
        {
            case window::DaemonRefKind::Local: localLabels.emplace(name); break;
            case window::DaemonRefKind::Host:
                if (!configured(name))
                    unconfiguredHosts.insert(daemon.value);
                break;
            case window::DaemonRefKind::Link: links.emplace_back(linkIdOf(name), daemon); break;
        }
    }

    auto const isWindowOn = [&](window::DaemonRef const& daemon) {
        return windowOn.contains(daemon.value);
    };
    auto const isChosen = [&](window::DaemonRef const& daemon, window::SidebarListingChoice configured) {
        return chosenListing(settings, daemon, configured) == window::SidebarListingChoice::Listed;
    };

    auto listing = Listing {};
    for (auto const& label: knownLocalLabels(inputs, std::move(localLabels)))
    {
        auto daemon = window::localDaemonRef(label);
        if (isWindowOn(daemon)
            || (isChosen(daemon, window::SidebarListingChoice::Listed)
                && !inputs.removedThisRun.contains(daemon.value)))
            listing.add(std::move(daemon), window::DaemonLocality::Local);
    }

    for (auto const& [name, choice]: hostsByName(settings))
    {
        auto daemon = window::hostDaemonRef(name);
        if (isWindowOn(daemon) || isChosen(daemon, choice))
            listing.add(std::move(daemon), window::DaemonLocality::Remote);
    }

    for (auto const& host: unconfiguredHosts)
        listing.add(window::DaemonRef { host }, window::DaemonLocality::Remote);

    std::ranges::stable_sort(links, std::less {}, [](auto const& entry) { return entry.first; });
    for (auto& entry: links)
        listing.add(std::move(entry.second), window::DaemonLocality::Remote);

    return listing.take();
}

window::SidebarListingChoice chosenListing(ListingSettings const& settings,
                                           window::DaemonRef const& daemon,
                                           window::SidebarListingChoice configured)
{
    if (settings.hiddenByUser.contains(daemon.value))
        return window::SidebarListingChoice::Unlisted;
    if (configured == window::SidebarListingChoice::Listed || settings.shownByUser.contains(daemon.value))
        return window::SidebarListingChoice::Listed;
    return window::SidebarListingChoice::Unlisted;
}

std::vector<window::HiddenDaemon> hiddenDaemons(PresenceInputs const& inputs,
                                                std::vector<ListedDaemon> const& listed)
{
    if (inputs.windowsOn.empty())
        return {};
    auto hidden = std::vector<window::HiddenDaemon> {};
    auto const add = [&](window::DaemonRef daemon, std::string name) {
        if (inputs.settings.hiddenByUser.contains(daemon.value)
            && !inputs.removedThisRun.contains(daemon.value)
            && !std::ranges::contains(listed, daemon, &ListedDaemon::daemon))
            hidden.push_back(window::HiddenDaemon { .daemon = std::move(daemon), .name = std::move(name) });
    };
    for (auto const& label: knownLocalLabels(inputs, {}))
        add(window::localDaemonRef(label), label);
    for (auto const& entry: hostsByName(inputs.settings))
        add(window::hostDaemonRef(entry.first), entry.first);
    return hidden;
}

window::DaemonRef daemonRefOf(DaemonAddress const& address, DaemonLinkId link)
{
    switch (address.kind)
    {
        case vthost::proto::SwitchDaemonKind::Name: return window::localDaemonRef(address.name);
        case vthost::proto::SwitchDaemonKind::Host: return window::hostDaemonRef(address.name);
        case vthost::proto::SwitchDaemonKind::Same: break;
    }
    return window::linkDaemonRef(unbox(link));
}

window::DaemonLinkState linkStateOf(vthost::client::ConnectionState state,
                                    std::optional<vthost::TransportErrorKind> lastFailure,
                                    ObserveSupport observe) noexcept
{
    if (observe == ObserveSupport::TooOld)
        return DaemonLinkState::TooOld;
    auto const& row = ConnectionRows[static_cast<size_t>(state)];
    switch (row.source)
    {
        case LinkStateSource::Connection: return row.state;
        case LinkStateSource::LastFailure: break;
    }
    return lastFailure.and_then(linkStateOfFailure).value_or(row.state);
}

} // namespace contour::remote
