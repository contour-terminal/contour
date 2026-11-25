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

    // Served from the latest range holding all of it whose bytes there are what flush() would leave: no other
    // range wrote there since it was fetched. Else fetched anew, with what flush() would leave overlaid.
    auto const settled = settledBytes(begin, end);
    auto const fresh = [&](Range const& range) {
        auto const rangeBegin = uint64_t { range.address };
        if (begin < rangeBegin || rangeBegin + range.bytes.size() < end)
            return false;
        auto const held = std::span { range.bytes }.subspan(begin - rangeBegin, size);
        return std::ranges::equal(
            held, settled, [](std::byte heldByte, std::optional<std::byte> settledByte) {
                return settledByte == heldByte;
            });
    };
    auto const served = std::ranges::find_if(_ranges | std::views::reverse, fresh);
    if (served != (_ranges | std::views::reverse).end())
        return std::span { served->bytes }.subspan(address - served->address, size);

    if (_heldBytes + size > _capBytes)
        return std::nullopt;
    auto fetched = _fetch(address, size);
    if (!fetched || fetched->size() != size)
        return std::nullopt;
    for (auto const [fetchedByte, settledByte]: std::views::zip(*fetched, settled))
        fetchedByte = settledByte.value_or(fetchedByte);

    auto snapshot = *fetched;
    _heldBytes += size;
    auto& added = _ranges.emplace_back(
        Range { .address = address, .bytes = std::move(*fetched), .snapshot = std::move(snapshot) });
    return std::span { added.bytes };
}

std::vector<std::optional<std::byte>> CopyPathMemory::settledBytes(uint64_t begin, uint64_t end) const
{
    // A byte's first holder fetched it unchanged; each later holder that changed it overrides it.
    auto settled = std::vector<std::optional<std::byte>>(end - begin);
    for (auto const& range: _ranges)
    {
        auto const rangeBegin = uint64_t { range.address };
        auto const overlapBegin = std::max(begin, rangeBegin);
        auto const overlapEnd = std::min(end, rangeBegin + range.bytes.size());
        for (auto const at: std::views::iota(overlapBegin, std::max(overlapBegin, overlapEnd)))
        {
            auto const index = at - rangeBegin;
            auto& byte = settled[at - begin];
            if (!byte || range.bytes[index] != range.snapshot[index])
                byte = range.bytes[index];
        }
    }
    return settled;
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
