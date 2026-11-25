// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What every remote backend of one kind shares: how often a runtime was brought up, how many are alive,
/// which threads are prepared for them, and which thread holds which lane.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>

namespace vtextension::wasm::remote
{

/// Why the calling thread got no lane.
enum class LaneRefusal : uint8_t
{
    Unprepared, ///< No guard of the kind's current generation prepares the thread.
    Exhausted,  ///< Every lane is held by another thread.
};

/// The state every remote backend of one kind shares.
///
/// The generation counts the bring-ups: each time the first runtime comes alive while none is. A guard only
/// remembers the generation it was entered for, as nothing in this process needs preparing; the hosts prepare
/// their own threads. A thread prepared by a guard takes a lane the first time it calls into an instance or
/// reads its memory, and gives it back with its outermost guard: a lane is a thread's own as long as it holds
/// it, so that its calls, the host calls they make and the calls made back from those all travel on it, and
/// no other thread's do. A thread's stamps are keyed by the kind's serial, never by its address.
class RemoteKindState: public std::enable_shared_from_this<RemoteKindState>
{
  public:
    /// Takes the next serial of the process.
    RemoteKindState() noexcept;

    /// Counts one more runtime alive, bringing the kind up anew if it is the first.
    void acquireRuntime() noexcept;

    /// Counts one runtime fewer alive.
    void releaseRuntime() noexcept;

    /// Prepares the calling thread for the current generation, unless it already is.
    /// @return The guard; `RuntimeUnavailable` while no runtime is alive.
    [[nodiscard]] std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread();

    /// @return Whether the calling thread is prepared for the current generation.
    [[nodiscard]] bool isCurrentThreadPrepared() const noexcept;

    /// @return The current generation; 0 before the first bring-up.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation.load(); }

    /// The calling thread's lane: the one it holds, or else a free one it takes until its outermost guard
    /// ends.
    /// @return The lane's index, below MaxLaneChannels; why the thread has none.
    [[nodiscard]] std::expected<std::size_t, LaneRefusal> laneOfCurrentThread();

    /// Gives a lane back. Called by the outermost guard holding it.
    /// @param lane The lane's index.
    void releaseLane(std::size_t lane) noexcept;

    /// Test hook.
    /// @return How many lanes a thread holds.
    [[nodiscard]] std::size_t lanesInUse() const noexcept;

    /// @return The kind's serial, unique in the process.
    [[nodiscard]] uint64_t serial() const noexcept { return _serial; }

  private:
    /// Whether a lane is held.
    enum class LaneState : uint8_t
    {
        Free,
        Held,
    };

    uint64_t _serial;
    mutable std::mutex _mutex;                        ///< Guards _liveRuntimes, _lanes and bring-ups.
    std::size_t _liveRuntimes = 0;                    ///< How many runtimes are alive.
    std::atomic<uint64_t> _generation {};             ///< The current bring-up generation.
    std::array<LaneState, MaxLaneChannels> _lanes {}; ///< Which lanes a thread holds.
};

} // namespace vtextension::wasm::remote
