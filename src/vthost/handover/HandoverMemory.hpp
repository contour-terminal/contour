// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <vthost/handover/HandoverCodec.hpp>

namespace vthost::handover
{

/// The largest handover image read() will take in. A ceiling against a corrupted size, not a judgement.
inline constexpr size_t MaxHandoverMemorySize = size_t { 1 } << 30;

/// The anonymous memory object a handover image is written into, and which the new process reads it back
/// from. An interface, so the orchestrator is tested without touching the operating system.
///
/// The descriptor write() returns has no name on disk. It is close-on-exec until keepAcrossExec(), so that
/// it is inherited by the one exec that should and by no child spawned in between.
class HandoverMemory
{
  public:
    HandoverMemory() = default;
    HandoverMemory(HandoverMemory const&) = delete;
    HandoverMemory(HandoverMemory&&) = delete;
    HandoverMemory& operator=(HandoverMemory const&) = delete;
    HandoverMemory& operator=(HandoverMemory&&) = delete;
    virtual ~HandoverMemory() = default;

    /// Stores @p image in a new anonymous memory object.
    /// @param image The bytes.
    /// @return The descriptor, which the caller owns and closes; or why it could not be made.
    [[nodiscard]] virtual std::expected<int, HandoverError> write(std::span<uint8_t const> image) = 0;

    /// Reads everything a memory object holds, from its start.
    /// @param fd A descriptor write() returned, in this or in the process that exec'd into this one.
    /// @return The bytes, or why they could not be read.
    [[nodiscard]] virtual std::expected<std::vector<uint8_t>, HandoverError> read(int fd) = 0;

    /// Makes @p fd survive the next exec, and only @p fd: clears its close-on-exec flag. Called once, right
    /// before the exec.
    /// @param fd A descriptor write() returned.
    /// @return Nothing, or why the flag could not be cleared.
    [[nodiscard]] virtual std::expected<void, HandoverError> keepAcrossExec(int fd) = 0;
};

/// @return The platform's memory object: memfd on Linux, an unlinked POSIX shared memory object on
///         macOS and the BSDs; on Windows one that refuses everything with NotSupported.
[[nodiscard]] std::unique_ptr<HandoverMemory> makeNativeHandoverMemory();

} // namespace vthost::handover
