// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An instance's linear memory as the parent sees it while its host copies memory rather than sharing it: the
/// ranges the parent read, kept until the instance next runs, and what the parent wrote into them.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::remote
{

/// Reads a range of an instance's memory from its host.
/// @return The range's bytes, exactly as many as asked for; nothing if the range lies outside the memory or
///         the host could not be reached.
using MemoryFetch = std::function<std::optional<std::vector<std::byte>>(uint32_t address, uint32_t size)>;

/// The ranges of one instance's memory the parent read since the instance last ran, as local copies it may
/// write to. A range is fetched once, and a view inside it is served from the copy. A range fetched while an
/// earlier one overlaps it starts out with the earlier one's bytes, written ones included, so that two views
/// of the same bytes agree when they are taken. What was written goes back to the instance (flush()) before
/// it next runs, after which the copies are dropped (clear()).
///
/// Not thread-safe: used by the thread calling into its instance.
class CopyPathMemory
{
  public:
    /// @param instanceId The instance the writes flush() returns are addressed to.
    /// @param capBytes The most bytes the copies may hold at once; a view that would take more is refused.
    /// @param fetch Where ranges not held yet are read from.
    CopyPathMemory(uint32_t instanceId, std::size_t capBytes, MemoryFetch fetch);

    /// A view of [@p address, @p address + @p size), served from a copy that holds all of it, or else from a
    /// range fetched for it. Valid until clear().
    /// @param address The guest address the view starts at.
    /// @param size The view's size in bytes; 0 yields an empty span, fetching nothing.
    /// @return The view; nothing if the range lies outside the memory or the 32-bit address space, could not
    ///         be fetched, or would take the copies past their cap.
    [[nodiscard]] std::optional<std::span<std::byte>> view(uint32_t address, uint32_t size);

    /// @return One write per maximal run of bytes changed since its range was fetched, range by range in the
    ///         order they were fetched: where two ranges overlap and both were written to, the later fetched
    ///         one's bytes land last.
    [[nodiscard]] std::vector<proto::MemoryWrite> flush() const;

    /// Drops every copy, and with them every view handed out.
    void clear() noexcept;

  private:
    /// One fetched range: what the parent sees and writes, and what it held when it was fetched.
    struct Range
    {
        uint32_t address = 0;            ///< Where it starts.
        std::vector<std::byte> bytes;    ///< Its bytes, as written since.
        std::vector<std::byte> snapshot; ///< Its bytes when fetched, earlier ranges' overlaid.
    };

    uint32_t _instanceId;
    std::size_t _capBytes;
    MemoryFetch _fetch;
    std::vector<Range> _ranges; ///< In the order they were fetched.
    std::size_t _heldBytes = 0; ///< What _ranges' bytes take together.
};

} // namespace vtextension::wasm::remote
