// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/CopyPathMemory.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace vtextension::wasm::remote
{

namespace
{
    /// The most bytes a 32-bit memory addresses.
    constexpr auto AddressSpaceBytes = std::size_t { 1 } << 32;
} // namespace

CopyPathMemory::CopyPathMemory(uint32_t instanceId, std::size_t memoryBytes, MemoryFetch fetch):
    _instanceId { instanceId },
    _memoryBytes { std::min(memoryBytes, AddressSpaceBytes) },
    _fetch { std::move(fetch) },
    // Default-initialized: nothing writes the buffer, so the system commits only the pages fetched into it.
    _shadow { std::make_unique_for_overwrite<std::byte[]>(_memoryBytes) },
    _slots((_memoryBytes + PageBytes - 1) / PageBytes, NoSlot)
{
}

std::expected<std::span<std::byte>, MemoryFailure> CopyPathMemory::view(uint32_t address, uint32_t size)
{
    if (size == 0)
        return std::span<std::byte> {};
    auto const begin = std::size_t { address };
    auto const end = begin + size; // cannot overflow: both are 32-bit
    if (end > _memoryBytes)
        return std::unexpected(MemoryFailure::OutOfBounds);

    // One fetch per run of adjacent absent pages.
    auto const firstPage = begin / PageBytes;
    auto const endPage = ((end - 1) / PageBytes) + 1;
    auto runBegin = std::optional<std::size_t> {};
    for (auto const page: std::views::iota(firstPage, endPage))
    {
        auto const absent = _slots[page] == NoSlot;
        if (absent && !runBegin)
            runBegin = page;
        else if (!absent && runBegin)
        {
            if (auto fetched = fetchPages(*std::exchange(runBegin, std::nullopt), page); !fetched)
                return std::unexpected(fetched.error());
        }
    }
    if (runBegin)
    {
        if (auto fetched = fetchPages(*runBegin, endPage); !fetched)
            return std::unexpected(fetched.error());
    }
    return std::span { _shadow.get(), _memoryBytes }.subspan(begin, size);
}

std::expected<void, MemoryFailure> CopyPathMemory::fetchPages(std::size_t first, std::size_t end)
{
    auto const address = first * PageBytes;
    auto const size = pageEnd(end - 1) - address;
    auto fetched = _fetch(static_cast<uint32_t>(address), static_cast<uint32_t>(size));
    if (!fetched)
        return std::unexpected(fetched.error());
    // A fetch answering other than the range asked for is refused as that range.
    if (fetched->size() != size)
        return std::unexpected(MemoryFailure::OutOfBounds);
    std::ranges::copy(*fetched, _shadow.get() + address);
    // Each page's snapshot takes the next slot of the arena, which keeps its capacity across clear().
    for (auto const page: std::views::iota(first, end))
    {
        _slots[page] = static_cast<uint32_t>(_fetched.size());
        _fetched.push_back(page);
        _snapshots.resize(_fetched.size() * PageBytes);
        auto const pageBytes =
            std::span { *fetched }.subspan((page - first) * PageBytes, pageEnd(page) - (page * PageBytes));
        std::ranges::copy(pageBytes, snapshotOf(page).begin());
    }
    return {};
}

std::span<std::byte> CopyPathMemory::snapshotOf(std::size_t page) noexcept
{
    return std::span { _snapshots }.subspan(std::size_t { _slots[page] } * PageBytes,
                                            pageEnd(page) - (page * PageBytes));
}

std::size_t CopyPathMemory::snapshotBytes() const noexcept
{
    return _snapshots.size();
}

std::size_t CopyPathMemory::pageEnd(std::size_t page) const noexcept
{
    return std::min((page + 1) * PageBytes, _memoryBytes);
}

std::vector<proto::MemoryWrite> CopyPathMemory::flush()
{
    auto const shadow = std::span { _shadow.get(), _memoryBytes };
    auto writes = std::vector<proto::MemoryWrite> {};
    // Emits [begin, end), and takes it into the snapshots of the pages it spans.
    auto const emit = [&](std::size_t begin, std::size_t end) {
        auto const run = shadow.subspan(begin, end - begin);
        writes.push_back(proto::MemoryWrite { .instanceId = _instanceId,
                                              .address = static_cast<uint32_t>(begin),
                                              .bytes = { run.begin(), run.end() } });
        for (auto const page: std::views::iota(begin / PageBytes, ((end - 1) / PageBytes) + 1))
        {
            auto const from = std::max(begin, page * PageBytes);
            auto const to = std::min(end, pageEnd(page));
            std::ranges::copy(shadow.subspan(from, to - from),
                              snapshotOf(page).begin()
                                  + static_cast<std::ptrdiff_t>(from - (page * PageBytes)));
        }
    };

    // A run of changed bytes continues across a page boundary only into the adjacent page.
    std::ranges::sort(_fetched);
    auto runBegin = std::optional<std::size_t> {};
    auto previousEnd = std::size_t { 0 };
    for (auto const page: _fetched)
    {
        auto const pageBegin = page * PageBytes;
        if (runBegin && pageBegin != previousEnd)
            emit(*std::exchange(runBegin, std::nullopt), previousEnd);
        previousEnd = pageEnd(page);
        // Most pages are only read: compared whole, at once, they end any run and need no closer look.
        auto const snapshot = snapshotOf(page);
        auto const current = shadow.subspan(pageBegin, snapshot.size());
        if (std::ranges::equal(current, snapshot))
        {
            if (runBegin)
                emit(*std::exchange(runBegin, std::nullopt), pageBegin);
            continue;
        }
        for (auto const at: std::views::iota(pageBegin, previousEnd))
        {
            auto const changed = shadow[at] != snapshot[at - pageBegin];
            if (changed && !runBegin)
                runBegin = at;
            else if (!changed && runBegin)
                emit(*std::exchange(runBegin, std::nullopt), at);
        }
    }
    if (runBegin)
        emit(*runBegin, previousEnd);
    return writes;
}

void CopyPathMemory::clear() noexcept
{
    for (auto const page: _fetched)
        _slots[page] = NoSlot;
    _fetched.clear();
    _snapshots.clear();
}

} // namespace vtextension::wasm::remote
