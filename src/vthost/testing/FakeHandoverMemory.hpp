// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Handover memory kept in this process, which touches no descriptor: what the live-upgrade orchestrator is
/// tested over.

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
    static constexpr int FirstDescriptor = 100000;

    [[nodiscard]] std::expected<int, handover::HandoverError> write(std::span<uint8_t const> image) override
    {
        if (onWrite)
            onWrite();
        if (writeError)
            return std::unexpected(*writeError);
        auto const fd = _next++;
        objects.emplace(fd, std::vector<uint8_t>(image.begin(), image.end()));
        return fd;
    }

    [[nodiscard]] std::expected<std::vector<uint8_t>, handover::HandoverError> read(int fd) override
    {
        auto const found = objects.find(fd);
        if (found == objects.end())
            return std::unexpected(handover::HandoverError::MemoryUnavailable);
        return found->second;
    }

    [[nodiscard]] std::expected<void, handover::HandoverError> keepAcrossExec(int fd) override
    {
        if (refusedDescriptor == fd)
            return std::unexpected(handover::HandoverError::MemoryUnavailable);
        kept.insert(fd);
        ++keepCalls;
        return {};
    }

    [[nodiscard]] std::expected<void, handover::HandoverError> closeOnExecAgain(int fd) override
    {
        kept.erase(fd);
        closedOnExecAgain.insert(fd);
        return {};
    }

    void discard(int fd) override
    {
        objects.erase(fd);
        discarded.insert(fd);
    }

    /// Runs as write() is called, before anything is stored: what makes a write throw.
    std::function<void()> onWrite;

    /// What every write() fails with, when set.
    std::optional<handover::HandoverError> writeError;

    /// The one descriptor keepAcrossExec() refuses, when set.
    std::optional<int> refusedDescriptor;

    /// Every memory object written and not discarded, by descriptor.
    std::map<int, std::vector<uint8_t>> objects;

    /// The descriptors that would survive the exec now.
    std::set<int> kept;

    /// How often keepAcrossExec() succeeded.
    std::size_t keepCalls = 0;

    /// Every descriptor closeOnExecAgain() was called for.
    std::set<int> closedOnExecAgain;

    /// Every descriptor discard() was called for.
    std::set<int> discarded;

  private:
    int _next = FirstDescriptor;
};

} // namespace vthost::testing
