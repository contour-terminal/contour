// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonDiscovery.hpp>

#include <algorithm>
#include <ranges>
#include <system_error>

#include <vthost/SocketPath.hpp>

namespace contour::cli
{

namespace
{
    class LocalDaemonDiscovery final: public IDaemonDiscovery
    {
      public:
        LocalDaemonDiscovery(std::filesystem::path socketDirectory,
                             vthost::DaemonSpawner& prober,
                             DaemonCounter counter,
                             DirectoryLister lister):
            _socketDirectory(std::move(socketDirectory)),
            _prober(prober),
            _counter(std::move(counter)),
            _lister(std::move(lister))
        {
        }

        [[nodiscard]] std::vector<DiscoveredDaemon> discover() override
        {
            auto entries = std::vector<std::pair<std::filesystem::path, vthost::DaemonPresence>> {};
            for (auto const& socket: _lister(_socketDirectory))
                if (isControlSocketName(socket))
                    entries.emplace_back(socket, _prober.probe(socket));
            return filterDiscovered(entries, _counter);
        }

      private:
        std::filesystem::path _socketDirectory;
        vthost::DaemonSpawner& _prober;
        DaemonCounter _counter;
        DirectoryLister _lister;
    };
} // namespace

DirectoryLister nativeDirectoryLister()
{
    return [](std::filesystem::path const& dir) {
        auto sockets = std::vector<std::filesystem::path> {};
        auto error = std::error_code {};
        for (auto const& entry: std::filesystem::directory_iterator(dir, error))
        {
            auto status = std::error_code {};
            // A socket on POSIX; Windows reports its AF_UNIX sockets as plain files.
#ifdef _WIN32
            if (entry.is_regular_file(status) || entry.is_socket(status))
#else
            if (entry.is_socket(status))
#endif
                sockets.push_back(entry.path());
        }
        return sockets;
    };
}

bool isControlSocketName(std::filesystem::path const& socketPath)
{
    auto const name = socketPath.filename().string();
    return !name.empty() && std::ranges::none_of(vthost::SiblingSuffixes, [&](std::string_view suffix) {
        return name.ends_with(suffix);
    });
}

std::vector<DiscoveredDaemon> filterDiscovered(
    std::vector<std::pair<std::filesystem::path, vthost::DaemonPresence>> const& entries,
    DaemonCounter const& counter)
{
    auto found = std::vector<DiscoveredDaemon> {};
    for (auto const& [socket, presence]: entries)
        if (presence == vthost::DaemonPresence::Serving && isControlSocketName(socket))
            found.push_back(DiscoveredDaemon {
                .label = socket.filename().string(), .socket = socket, .workspaces = counter(socket) });
    // `default` first: it is the daemon a plain launch uses.
    std::ranges::sort(found, [](DiscoveredDaemon const& a, DiscoveredDaemon const& b) {
        auto const isDefault = [](DiscoveredDaemon const& daemon) {
            return daemon.label == vthost::DefaultSocketLabel;
        };
        return std::pair { !isDefault(a), a.label } < std::pair { !isDefault(b), b.label };
    });
    return found;
}

std::unique_ptr<IDaemonDiscovery> makeLocalDaemonDiscovery(std::filesystem::path socketDirectory,
                                                           vthost::DaemonSpawner& prober,
                                                           DaemonCounter counter,
                                                           DirectoryLister lister)
{
    return std::make_unique<LocalDaemonDiscovery>(
        std::move(socketDirectory), prober, std::move(counter), std::move(lister));
}

} // namespace contour::cli
