// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An instance's linear memory as the parent sees it while its host copies memory rather than sharing it: a
/// shadow of the whole memory, filled page by page as the parent reads it, until the instance next runs.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
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

/// The parts of one instance's memory the parent read since the instance last ran, in a shadow of the whole
/// memory, at the same offsets. A page is fetched the first time a view reaches it, and every view of the
/// same bytes is the same shadow memory: a write through one view is what every other view of those bytes
/// reads. What was written goes back to the instance (flush()) before it next runs, after which the fetched
/// pages are forgotten (clear()).
///
/// The shadow is allocated once and left untouched until a page is fetched into it, so that only the pages
/// read take memory where the system commits it lazily (Linux, macOS, the BSDs). Windows commits the
/// allocation up front, charging it against the commit limit while its pages stay unused.
///
/// Not thread-safe: used by the thread calling into its instance.
class CopyPathMemory
{
  public:
    /// The granularity pages are fetched at, in bytes.
    static constexpr auto PageBytes = std::size_t { 4096 };

    /// @param instanceId The instance the writes flush() returns are addressed to.
    /// @param memoryBytes The largest the instance's memory may be; no view reaches past it.
    /// @param fetch Where pages not held yet are read from.
    CopyPathMemory(uint32_t instanceId, std::size_t memoryBytes, MemoryFetch fetch);

    /// A view of [@p address, @p address + @p size) in the shadow, fetching the pages of it not held yet: one
    /// fetch per run of adjacent missing pages. Valid until the CopyPathMemory goes; its bytes are the
    /// instance's only until clear().
    /// @param address The guest address the view starts at.
    /// @param size The view's size in bytes; 0 yields an empty span, fetching nothing.
    /// @return The view; nothing if the range reaches past the memory's limit or could not be fetched.
    [[nodiscard]] std::optional<std::span<std::byte>> view(uint32_t address, uint32_t size);

    /// @return One write per maximal run of bytes changed since they were fetched or last flushed, in address
    ///         order. What they return counts as unchanged from then on.
    [[nodiscard]] std::vector<proto::MemoryWrite> flush();

    /// Forgets every fetched page: the next view of it fetches it again.
    void clear() noexcept;

  private:
    /// Whether a page of the shadow holds the instance's bytes.
    enum class PageState : uint8_t
    {
        Absent,  ///< Not fetched since the instance last ran.
        Fetched, ///< Fetched: the shadow holds it, as written since.
    };

    /// Why pages could not be fetched.
    enum class FetchError : uint8_t
    {
        Unreachable, ///< The host refused the range, or could not be reached.
    };

    /// Fetches pages [@p first, @p end) into the shadow and their snapshot, all of them absent.
    [[nodiscard]] std::expected<void, FetchError> fetchPages(std::size_t first, std::size_t end);

    /// @return The address one past page @p page's last byte.
    [[nodiscard]] std::size_t pageEnd(std::size_t page) const noexcept;

    uint32_t _instanceId;
    std::size_t _memoryBytes;
    MemoryFetch _fetch;
    std::unique_ptr<std::byte[]> _shadow;   ///< The memory as the parent sees and writes it.
    std::unique_ptr<std::byte[]> _snapshot; ///< The fetched pages as fetched or last flushed.
    std::vector<PageState> _pages;          ///< Per page of the memory.
    std::vector<std::size_t> _fetched;      ///< The pages fetched since clear(), in fetch order.
};

} // namespace vtextension::wasm::remote
