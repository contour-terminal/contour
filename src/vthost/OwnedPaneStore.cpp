// SPDX-License-Identifier: Apache-2.0
#include <vthost/OwnedPaneStore.hpp>

#include <core/Utf8Path.hpp>

#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/Logging.hpp>

namespace vthost
{

std::vector<OwnedPaneRecord> parseOwnedPanes(std::string_view text)
{
    auto records = std::vector<OwnedPaneRecord> {};
    for (auto const line: text | std::views::split('\n'))
    {
        auto const entry = std::string_view { line.begin(), line.end() };
        auto const space = entry.find(' ');
        if (space == std::string_view::npos)
            continue;
        auto const pane = vtcontrol::parseUuid(entry.substr(0, space));
        auto const tag = entry.substr(space + 1);
        if (!pane || tag.empty())
            continue;
        records.push_back(OwnedPaneRecord { .pane = *pane, .tag = std::string { tag } });
    }
    return records;
}

std::string formatOwnedPanes(std::span<OwnedPaneRecord const> records)
{
    auto text = std::string {};
    for (auto const& record: records)
        text += std::format("{} {}\n", vtcontrol::toString(record.pane), record.tag);
    return text;
}

FileOwnedPaneStore::FileOwnedPaneStore(core::platform::FileSystem const& fileSystem,
                                       std::map<std::string, std::filesystem::path, std::less<>> files):
    _fileSystem { fileSystem }, _files { std::move(files) }
{
}

std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> FileOwnedPaneStore::loadAll() const
{
    auto all = std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> {};
    for (auto const& [extension, file]: _files)
        if (auto const text = _fileSystem.readFile(file); text)
            all.emplace_back(extension, parseOwnedPanes(*text));
    return all;
}

std::expected<void, OwnedPaneStoreError> FileOwnedPaneStore::save(std::string_view extension,
                                                                  std::span<OwnedPaneRecord const> records)
{
    auto const found = _files.find(extension);
    if (found == _files.end())
        return std::unexpected(OwnedPaneStoreError::Unnamed);
    auto const& file = found->second;
    auto const directory = file.parent_path();
    auto temporary = file;
    temporary += ".tmp";
    // Whole or not at all, as the store and the snapshot are written: created exclusively after dropping what
    // a crash left, synced, renamed over the file, and the rename synced.
    std::ignore = _fileSystem.remove(temporary);
    auto const written =
        _fileSystem.createDirectories(directory)
            .and_then([&] { return _fileSystem.createNewFile(temporary); })
            .and_then([&] { return _fileSystem.writeFile(temporary, formatOwnedPanes(records)); })
            .and_then([&] { return _fileSystem.syncFile(temporary); })
            .and_then([&] { return _fileSystem.replaceFile(temporary, file); })
            .and_then([&] { return _fileSystem.syncDirectory(directory); });
    if (written)
        return {};
    std::ignore = _fileSystem.remove(temporary);
    errorLog()(
        "the owned panes of {} cannot be kept in {}: {}", extension, core::pathToUtf8(file), written.error());
    return std::unexpected(OwnedPaneStoreError::WriteFailed);
}

AsyncOwnedPaneStore::AsyncOwnedPaneStore(IOwnedPaneStore& store): _store { store }
{
}

AsyncOwnedPaneStore::~AsyncOwnedPaneStore()
{
    // What was saved reaches the store before the thread goes; the jthread then stops and joins.
    flush();
}

std::vector<std::pair<std::string, std::vector<OwnedPaneRecord>>> AsyncOwnedPaneStore::loadAll() const
{
    return _store.loadAll();
}

std::expected<void, OwnedPaneStoreError> AsyncOwnedPaneStore::save(std::string_view extension,
                                                                   std::span<OwnedPaneRecord const> records)
{
    {
        auto const lock = std::scoped_lock { _mutex };
        _pending.insert_or_assign(std::string { extension },
                                  std::vector<OwnedPaneRecord>(records.begin(), records.end()));
        ++_submitted;
        if (!_thread.joinable())
            _thread = std::jthread { [this](std::stop_token const& stop) { run(stop); } };
    }
    _changed.notify_all();
    return {};
}

void AsyncOwnedPaneStore::flush()
{
    auto lock = std::unique_lock { _mutex };
    if (!_thread.joinable())
        return;
    auto const target = _submitted;
    _changed.wait(lock, [&] { return _written >= target; });
}

void AsyncOwnedPaneStore::run(std::stop_token const& stop)
{
    auto lock = std::unique_lock { _mutex };
    // Ends only once stopped with nothing pending: the destructor flushes before it stops.
    while (_changed.wait(lock, stop, [&] { return !_pending.empty(); }))
    {
        auto jobs = std::exchange(_pending, {});
        auto const generation = _submitted;
        lock.unlock();
        for (auto const& [extension, records]: jobs)
            std::ignore = _store.save(extension, records);
        lock.lock();
        _written = generation;
        _changed.notify_all();
    }
}

} // namespace vthost
