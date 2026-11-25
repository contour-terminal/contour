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
    // Default-initialized: nothing writes the buffers, so the system commits only the pages fetched into
    // them.
    _shadow { std::make_unique_for_overwrite<std::byte[]>(_memoryBytes) },
    _snapshot { std::make_unique_for_overwrite<std::byte[]>(_memoryBytes) },
    _pages((_memoryBytes + PageBytes - 1) / PageBytes, PageState::Absent)
{
}

std::optional<std::span<std::byte>> CopyPathMemory::view(uint32_t address, uint32_t size)
{
    if (size == 0)
        return std::span<std::byte> {};
    auto const begin = std::size_t { address };
    auto const end = begin + size; // cannot overflow: both are 32-bit
    if (end > _memoryBytes)
        return std::nullopt;

    // One fetch per run of adjacent absent pages.
    auto const firstPage = begin / PageBytes;
    auto const endPage = ((end - 1) / PageBytes) + 1;
    auto runBegin = std::optional<std::size_t> {};
    for (auto const page: std::views::iota(firstPage, endPage))
    {
        auto const absent = _pages[page] == PageState::Absent;
        if (absent && !runBegin)
            runBegin = page;
        else if (!absent && runBegin && !fetchPages(*std::exchange(runBegin, std::nullopt), page))
            return std::nullopt;
    }
    if (runBegin && !fetchPages(*runBegin, endPage))
        return std::nullopt;
    return std::span { _shadow.get(), _memoryBytes }.subspan(begin, size);
}

std::expected<void, CopyPathMemory::FetchError> CopyPathMemory::fetchPages(std::size_t first, std::size_t end)
{
    auto const address = first * PageBytes;
    auto const size = pageEnd(end - 1) - address;
    auto fetched = _fetch(static_cast<uint32_t>(address), static_cast<uint32_t>(size));
    if (!fetched || fetched->size() != size)
        return std::unexpected(FetchError::Unreachable);
    std::ranges::copy(*fetched, _shadow.get() + address);
    std::ranges::copy(*fetched, _snapshot.get() + address);
    for (auto const page: std::views::iota(first, end))
    {
        _pages[page] = PageState::Fetched;
        _fetched.push_back(page);
    }
    return {};
}

std::size_t CopyPathMemory::pageEnd(std::size_t page) const noexcept
{
    return std::min((page + 1) * PageBytes, _memoryBytes);
}

std::vector<proto::MemoryWrite> CopyPathMemory::flush()
{
    auto const shadow = std::span { _shadow.get(), _memoryBytes };
    auto const snapshot = std::span { _snapshot.get(), _memoryBytes };
    auto writes = std::vector<proto::MemoryWrite> {};
    auto const emit = [&](std::size_t begin, std::size_t end) {
        auto const run = shadow.subspan(begin, end - begin);
        writes.push_back(proto::MemoryWrite { .instanceId = _instanceId,
                                              .address = static_cast<uint32_t>(begin),
                                              .bytes = { run.begin(), run.end() } });
        std::ranges::copy(run, snapshot.begin() + static_cast<std::ptrdiff_t>(begin));
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
        auto const extent = previousEnd - pageBegin;
        if (std::ranges::equal(shadow.subspan(pageBegin, extent), snapshot.subspan(pageBegin, extent)))
        {
            if (runBegin)
                emit(*std::exchange(runBegin, std::nullopt), pageBegin);
            continue;
        }
        for (auto const at: std::views::iota(pageBegin, previousEnd))
        {
            auto const changed = shadow[at] != snapshot[at];
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
        _pages[page] = PageState::Absent;
    _fetched.clear();
}

} // namespace vtextension::wasm::remote
