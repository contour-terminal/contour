// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Handover memory kept in this process, which touches no descriptor: what the live-upgrade orchestrator is
/// tested over.

#include <vtpty/CrossingHandle.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include <vthost/handover/HandoverMemory.hpp>

namespace vthost::testing
{

/// HandoverMemory that stores each image under a made-up descriptor number, and records which descriptors
/// were made to survive the exec instead of changing any flag of theirs.
class FakeHandoverMemory final: public handover::HandoverMemory
{
  public:
    /// The first descriptor number write() hands out: above any this process opens in a test.
    static constexpr auto FirstDescriptor = decltype(vtpty::CrossingHandle::value) { 100000 };

    [[nodiscard]] std::expected<vtpty::CrossingHandle, handover::HandoverError> write(
        std::span<uint8_t const> image) override
    {
        if (onWrite)
            onWrite();
        if (writeError)
            return std::unexpected(*writeError);
        auto const handle = vtpty::CrossingHandle { _next++ };
        objects.emplace(handle, std::vector<uint8_t>(image.begin(), image.end()));
        return handle;
    }

    [[nodiscard]] std::expected<std::vector<uint8_t>, handover::HandoverError> read(
        vtpty::CrossingHandle handle) override
    {
        auto const found = objects.find(handle);
        if (found == objects.end())
            return std::unexpected(handover::HandoverError::MemoryUnavailable);
        return found->second;
    }

    [[nodiscard]] std::expected<void, handover::HandoverError> keepAcrossExec(
        vtpty::CrossingHandle handle) override
    {
        if (refusedDescriptor == handle)
            return std::unexpected(handover::HandoverError::MemoryUnavailable);
        kept.insert(handle);
        ++keepCalls;
        return {};
    }

    [[nodiscard]] std::expected<void, handover::HandoverError> closeOnExecAgain(
        vtpty::CrossingHandle handle) override
    {
        kept.erase(handle);
        closedOnExecAgain.insert(handle);
        return {};
    }

    void discard(vtpty::CrossingHandle handle) override
    {
        objects.erase(handle);
        discarded.insert(handle);
    }

    /// Runs as write() is called, before anything is stored: what makes a write throw.
    std::function<void()> onWrite;

    /// What every write() fails with, when set.
    std::optional<handover::HandoverError> writeError;

    /// The one descriptor keepAcrossExec() refuses, when set.
    std::optional<vtpty::CrossingHandle> refusedDescriptor;

    /// Every memory object written and not discarded, by descriptor.
    std::map<vtpty::CrossingHandle, std::vector<uint8_t>> objects;

    /// The descriptors that would survive the exec now.
    std::set<vtpty::CrossingHandle> kept;

    /// How often keepAcrossExec() succeeded.
    std::size_t keepCalls = 0;

    /// Every descriptor closeOnExecAgain() was called for.
    std::set<vtpty::CrossingHandle> closedOnExecAgain;

    /// Every descriptor discard() was called for.
    std::set<vtpty::CrossingHandle> discarded;

  private:
    decltype(vtpty::CrossingHandle::value) _next = FirstDescriptor;
};

} // namespace vthost::testing
