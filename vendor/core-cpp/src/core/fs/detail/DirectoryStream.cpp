// SPDX-License-Identifier: Apache-2.0
#include <core/fs/detail/DirectoryStream.hpp>

#include <core/async/Task.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>

#include <expected>
#include <optional>
#include <utility>

namespace core::fs::detail
{

async::Task<FsResult<std::optional<DirectoryEntry>>> BatchedDirectoryStream::next()
{
    // A batch may hold no entry and still not end the directory (one that read only `.` and `..`).
    while (_position == _entries.size())
    {
        if (_end == StreamEnd::Reached)
            co_return std::optional<DirectoryEntry> {};
        auto batch = co_await fetch();
        _entries.clear();
        _position = 0;
        if (!batch)
        {
            _end = StreamEnd::Reached;
            co_return std::unexpected { std::move(batch.error()) };
        }
        _entries = std::move(batch->entries);
        _end = batch->end;
    }
    auto entry = std::move(_entries[_position]);
    ++_position;
    if (!entry)
        co_return std::unexpected { std::move(entry.error()) };
    co_return std::optional { std::move(*entry) };
}

} // namespace core::fs::detail
