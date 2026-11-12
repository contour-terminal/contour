// SPDX-License-Identifier: Apache-2.0
#include <vthost/KnownDaemons.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/DaemonSpawn.hpp>

namespace vthost
{

namespace
{
    /// @param line One line of the file.
    /// @return `HOST:PORT FINGERPRINT`, or nullopt for a blank, a comment or a line that is neither.
    [[nodiscard]] std::optional<DaemonPin> parseLine(std::string_view line)
    {
        // Any whitespace separates, and none is part of a field: a file edited on Windows ends its lines in
        // CR, which must not end up in the fingerprint.
        auto const isBlank = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
        };
        auto parts = std::vector<std::string_view> {};
        for (auto const run:
             line | std::views::chunk_by([&](char a, char b) { return isBlank(a) == isBlank(b); }))
            if (!isBlank(run.front()))
                parts.emplace_back(run.begin(), run.end());
        if (parts.size() != 2 || parts.front().starts_with('#') || !parts.back().starts_with("SHA256:"))
            return std::nullopt;
        return DaemonPin { .hostPort = std::string { parts.front() },
                           .fingerprint = std::string { parts.back() } };
    }

    /// Whether @p file is shown not to be there: the nearest directory above it that is there was listed, and
    /// holds no entry on the way to it. A directory that cannot be listed or searched shows nothing, and is
    /// an error rather than an absence: a permission problem must not pass for "no file".
    /// @param fileSystem What is asked.
    /// @param file The file.
    /// @return Whether it is absent, or why that cannot be told.
    [[nodiscard]] std::expected<bool, std::string> isAbsent(core::platform::FileSystem const& fileSystem,
                                                            std::filesystem::path const& file)
    {
        auto child = file;
        auto directory = file.parent_path().empty() ? std::filesystem::path { "." } : file.parent_path();
        // A missing directory is as good as a missing file: the listing is of the nearest one that is there.
        while (!fileSystem.exists(directory))
        {
            if (directory == directory.parent_path())
                return true; // nothing at all is there, not even a root
            child = directory;
            directory = directory.parent_path();
        }
        return fileSystem.listDirectory(directory).transform(
            [&](std::vector<core::platform::FileSystem::DirectoryEntry> const& entries) {
                return std::ranges::none_of(
                    entries, [&](auto const& entry) { return entry.path.filename() == child.filename(); });
            });
    }

    /// @param content The whole file.
    /// @return Every line that parses, in order.
    [[nodiscard]] std::vector<DaemonPin> parseFile(std::string_view content)
    {
        auto pins = std::vector<DaemonPin> {};
        for (auto const line: content | std::views::split('\n'))
            if (auto pin = parseLine(std::string_view { line.begin(), line.end() }))
                pins.push_back(std::move(*pin));
        return pins;
    }
} // namespace

std::string pinKey(std::string_view host, std::uint16_t port)
{
    if (host.contains(':'))
        return std::format("[{}]:{}", host, port);
    return std::format("{}:{}", host, port);
}

std::optional<std::string> pinKeyOf(std::string_view spec)
{
    return parseHostPort(spec).transform(
        [](auto const& hostPort) { return pinKey(hostPort.first, hostPort.second); });
}

KnownDaemons::KnownDaemons(core::platform::FileSystem const& fileSystem,
                           std::filesystem::path file,
                           core::IRandom& random):
    _fileSystem(fileSystem), _file(std::move(file)), _random(random)
{
}

std::expected<std::optional<std::string>, std::string> KnownDaemons::pinOf(std::string_view hostPort) const
{
    return load().transform([&](std::vector<DaemonPin> const& pins) -> std::optional<std::string> {
        // The last line wins, as a later pin replaced an earlier one.
        for (auto const& entry: pins | std::views::reverse)
            if (entry.hostPort == hostPort)
                return entry.fingerprint;
        return std::nullopt;
    });
}

std::expected<std::vector<DaemonPin>, std::string> KnownDaemons::load() const
{
    auto const name = core::pathToUtf8(_file);
    // Read as empty by some file systems, a directory would pass for a file without pins.
    if (_fileSystem.isDirectory(_file))
        return std::unexpected(std::format("'{}' is a directory, not a file", name));
    auto content = _fileSystem.readFile(_file);
    if (content)
        return parseFile(*content);
    // Only a file that is shown not to be there is no pins. One that cannot be read is not, or every daemon
    // would be asked about anew, and rewriting it would drop every pin in it.
    // The file system's own words name the file already, and are not repeated.
    return isAbsent(_fileSystem, _file)
        .transform_error([&](std::string const& why) {
            return std::format("whether '{}' is there cannot be told: {}", name, why);
        })
        .and_then([&](bool absent) -> std::expected<std::vector<DaemonPin>, std::string> {
            if (absent)
                return std::vector<DaemonPin> {};
            return std::unexpected(std::format("'{}' is there but cannot be read", name));
        });
}

std::expected<void, std::string> KnownDaemons::store(std::vector<DaemonPin> const& entries) const
{
    auto content = std::string {};
    for (auto const& entry: entries)
        content += std::format("{} {}\n", entry.hostPort, entry.fingerprint);
    // Whole or not at all: a reader never sees a truncated file, and a crash leaves the last good one.
    // Under a name of its own: another writer's temporary file is never touched, so two writers cannot end up
    // writing one file. A crash may leave one behind, which is harmless.
    auto suffix = std::array<std::byte, 8> {};
    _random.fill(suffix);
    auto temporary = _file;
    temporary += std::format(
        ".{}.tmp",
        core::toHexString(std::string_view { reinterpret_cast<char const*>(suffix.data()), suffix.size() }));
    return _fileSystem.createDirectories(_file.parent_path())
        .and_then([&] { return _fileSystem.createNewFile(temporary); })
        .and_then([&] { return _fileSystem.writeFile(temporary, content); })
        .and_then([&] { return _fileSystem.syncFile(temporary); })
        .and_then([&] { return _fileSystem.replaceFile(temporary, _file); })
        .transform_error([&](std::string why) {
            std::ignore = _fileSystem.remove(temporary);
            return std::format("cannot keep '{}': {}", core::pathToUtf8(_file), why);
        });
}

std::expected<void, std::string> KnownDaemons::pin(std::string_view hostPort, std::string_view fingerprint)
{
    return load().and_then([&](std::vector<DaemonPin> entries) {
        std::erase_if(entries, [&](DaemonPin const& entry) { return entry.hostPort == hostPort; });
        entries.push_back(
            DaemonPin { .hostPort = std::string { hostPort }, .fingerprint = std::string { fingerprint } });
        return store(entries);
    });
}

std::expected<bool, std::string> KnownDaemons::forget(std::string_view hostPort)
{
    return load().and_then([&](std::vector<DaemonPin> entries) -> std::expected<bool, std::string> {
        auto const removed =
            std::erase_if(entries, [&](DaemonPin const& entry) { return entry.hostPort == hostPort; });
        if (removed == 0)
            return false;
        return store(entries).transform([] { return true; });
    });
}

} // namespace vthost
