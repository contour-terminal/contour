// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/RemoteKindState.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/RemoteBackend.hpp>

namespace vtextension::wasm::remote
{

namespace
{
    /// The serial the next kind takes; 0 names none.
    auto nextSerial = std::atomic<uint64_t> { 1 };

    /// What the calling thread is prepared for, by one kind.
    struct ThreadStamp
    {
        uint64_t kind = 0;               ///< The serial of the kind whose guard prepared the thread.
        uint64_t generation = 0;         ///< The generation it was prepared for.
        std::optional<std::size_t> lane; ///< The lane of that kind the thread holds, if it took one.
    };

    /// The calling thread's stamps, one per kind it is prepared by, in the order it was first prepared.
    thread_local auto threadStamps = std::vector<ThreadStamp> {};

    /// @return The calling thread's stamp for the kind of @p serial; null if it has none.
    [[nodiscard]] ThreadStamp* stampOf(uint64_t serial) noexcept
    {
        auto const found = std::ranges::find(threadStamps, serial, &ThreadStamp::kind);
        return found != threadStamps.end() ? &*found : nullptr;
    }

    /// Whether a guard is the one that prepared its thread, or nests inside that one.
    enum class GuardRole : uint8_t
    {
        Nested,    ///< The thread was prepared already; ending the guard leaves it so.
        Outermost, ///< The guard prepared the thread; ending it ends the preparation, and frees its lane.
    };

    /// A thread's preparation: a stamp of the generation it was entered for.
    class RemoteThreadGuard final: public IThreadGuard
    {
      public:
        RemoteThreadGuard(std::shared_ptr<RemoteKindState> kind, uint64_t generation, GuardRole role) noexcept
            :
            _kind { std::move(kind) }, _generation { generation }, _role { role }
        {
        }

        RemoteThreadGuard(RemoteThreadGuard const&) = delete;
        RemoteThreadGuard(RemoteThreadGuard&&) = delete;
        RemoteThreadGuard& operator=(RemoteThreadGuard const&) = delete;
        RemoteThreadGuard& operator=(RemoteThreadGuard&&) = delete;

        ~RemoteThreadGuard() override
        {
            if (_role != GuardRole::Outermost)
                return;
            // A guard entered for an earlier generation leaves alone the stamp a later guard renewed.
            auto* const stamp = stampOf(_kind->serial());
            if (!stamp || stamp->generation != _generation)
                return;
            if (stamp->lane)
                _kind->releaseLane(*stamp->lane);
            std::erase_if(threadStamps,
                          [this](ThreadStamp const& entry) { return entry.kind == _kind->serial(); });
        }

        [[nodiscard]] uint64_t generation() const noexcept override { return _generation; }

      private:
        std::shared_ptr<RemoteKindState> _kind;
        uint64_t _generation;
        GuardRole _role;
    };
} // namespace

RemoteKindState::RemoteKindState() noexcept: _serial { nextSerial.fetch_add(1) }
{
}

void RemoteKindState::acquireRuntime() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    if (_liveRuntimes++ == 0)
        ++_generation;
}

void RemoteKindState::releaseRuntime() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    --_liveRuntimes;
}

std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> RemoteKindState::enterThread()
{
    auto const lock = std::scoped_lock { _mutex };
    if (_liveRuntimes == 0)
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    auto const current = _generation.load();
    auto* stamp = stampOf(_serial);
    if (stamp && stamp->generation == current)
        return std::make_unique<RemoteThreadGuard>(shared_from_this(), current, GuardRole::Nested);
    if (stamp)
    {
        // A stamp of an earlier generation is renewed; the lane it held goes back first.
        if (stamp->lane)
            _lanes.at(*stamp->lane) = LaneState::Free;
        *stamp = ThreadStamp { .kind = _serial, .generation = current, .lane = std::nullopt };
    }
    else
        threadStamps.push_back(ThreadStamp { .kind = _serial, .generation = current, .lane = std::nullopt });
    return std::make_unique<RemoteThreadGuard>(shared_from_this(), current, GuardRole::Outermost);
}

bool RemoteKindState::isCurrentThreadPrepared() const noexcept
{
    auto const* const stamp = stampOf(_serial);
    return stamp && stamp->generation != 0 && stamp->generation == _generation.load();
}

std::expected<std::size_t, LaneRefusal> RemoteKindState::laneOfCurrentThread()
{
    if (!isCurrentThreadPrepared())
        return std::unexpected(LaneRefusal::Unprepared);
    auto& stamp = *stampOf(_serial);
    if (stamp.lane)
        return *stamp.lane;
    auto const lock = std::scoped_lock { _mutex };
    auto const free = std::ranges::find(_lanes, LaneState::Free);
    if (free == _lanes.end())
    {
        errorLog()("Extensions run on more than {} threads at once; a call on one more fails.",
                   MaxLaneChannels);
        return std::unexpected(LaneRefusal::Exhausted);
    }
    *free = LaneState::Held;
    stamp.lane = static_cast<std::size_t>(free - _lanes.begin());
    return *stamp.lane;
}

void RemoteKindState::releaseLane(std::size_t lane) noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    _lanes.at(lane) = LaneState::Free;
}

std::size_t RemoteKindState::lanesInUse() const noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    return static_cast<std::size_t>(std::ranges::count(_lanes, LaneState::Held));
}

std::shared_ptr<RemoteKindState> makeRemoteKindState()
{
    return std::make_shared<RemoteKindState>();
}

uint64_t preparedGeneration() noexcept
{
    return threadStamps.empty() ? 0 : threadStamps.back().generation;
}

} // namespace vtextension::wasm::remote
