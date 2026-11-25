// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/CopyPathMemory.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace vtextension::wasm::remote
{

CopyPathMemory::CopyPathMemory(uint32_t instanceId, std::size_t capBytes, MemoryFetch fetch):
    _instanceId { instanceId }, _capBytes { capBytes }, _fetch { std::move(fetch) }
{
}

std::optional<std::span<std::byte>> CopyPathMemory::view(uint32_t address, uint32_t size)
{
    if (size == 0)
        return std::span<std::byte> {};
    auto const begin = uint64_t { address };
    auto const end = begin + size;
    if (end > uint64_t { 1 } << 32)
        return std::nullopt;

    // The latest range holding all of it: it carries every earlier range's bytes it overlaps.
    for (auto& range: _ranges | std::views::reverse)
        if (range.address <= begin && end <= range.address + uint64_t { range.bytes.size() })
            return std::span { range.bytes }.subspan(address - range.address, size);

    if (_heldBytes + size > _capBytes)
        return std::nullopt;
    auto fetched = _fetch(address, size);
    if (!fetched || fetched->size() != size)
        return std::nullopt;

    // Earlier ranges' bytes, as written, overlaid in fetch order: what was written since they were fetched
    // is what the instance would hold now, had it been written there.
    for (auto const& earlier: _ranges)
    {
        auto const earlierBegin = uint64_t { earlier.address };
        auto const earlierEnd = earlierBegin + earlier.bytes.size();
        auto const overlapBegin = std::max(begin, earlierBegin);
        auto const overlapEnd = std::min(end, earlierEnd);
        if (overlapBegin >= overlapEnd)
            continue;
        auto const source =
            std::span { earlier.bytes }.subspan(overlapBegin - earlierBegin, overlapEnd - overlapBegin);
        std::ranges::copy(source, fetched->begin() + static_cast<std::ptrdiff_t>(overlapBegin - begin));
    }

    auto snapshot = *fetched;
    _heldBytes += size;
    auto& added = _ranges.emplace_back(
        Range { .address = address, .bytes = std::move(*fetched), .snapshot = std::move(snapshot) });
    return std::span { added.bytes };
}

std::vector<proto::MemoryWrite> CopyPathMemory::flush() const
{
    auto writes = std::vector<proto::MemoryWrite> {};
    for (auto const& range: _ranges)
    {
        auto const emit = [&](std::size_t begin, std::size_t end) {
            auto const run = std::span { range.bytes }.subspan(begin, end - begin);
            writes.push_back(proto::MemoryWrite { .instanceId = _instanceId,
                                                  .address = range.address + static_cast<uint32_t>(begin),
                                                  .bytes = { run.begin(), run.end() } });
        };
        auto runBegin = std::optional<std::size_t> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, range.bytes.size()))
        {
            auto const changed = range.bytes[index] != range.snapshot[index];
            if (changed && !runBegin)
                runBegin = index;
            else if (!changed && runBegin)
                emit(*std::exchange(runBegin, std::nullopt), index);
        }
        if (runBegin)
            emit(*runBegin, range.bytes.size());
    }
    return writes;
}

void CopyPathMemory::clear() noexcept
{
    _ranges.clear();
    _heldBytes = 0;
}

} // namespace vtextension::wasm::remote
