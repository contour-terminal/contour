// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the supervisor's backstop sees of an extension host's lanes: which of the parent's frames run on
/// each, which of them an interrupt armed, and whether the innermost one waits on the parent's host call.
///
/// A lane's thread records its frames as it runs them; any thread arms one by interrupting its instance; the
/// supervisor observes, and gives a host whose armed frame does not end within the grace a kill. Everything
/// here is atomics, and nothing waits: interrupt() runs on the watchdog, which must never be stalled.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

namespace vtextension::wasm::remote
{

/// How many frames of a lane are recorded: as many guest calls as nest (MaxCallDepth), and a memory read the
/// innermost one's host call makes. Deeper frames are only counted, and cannot be armed.
inline constexpr std::size_t LaneFrameCapacity = proto::MaxCallDepth + 1;

/// Whether an interrupt armed a frame on the lane.
enum class LaneArming : uint8_t
{
    Disarmed, ///< No frame on the lane was interrupted.
    Armed,    ///< A frame on the lane was interrupted, and has not ended.
};

/// Who the lane waits on.
enum class LanePlace : uint8_t
{
    Idle,        ///< No frame runs on it.
    InHelper,    ///< Its innermost frame waits on the host: the host's time.
    ServingHost, ///< Its innermost frame runs a host call in the parent: the parent's time.
};

/// One look at a lane.
struct LaneObservation
{
    /// Names the arming in force: it changes whenever another frame is armed, or the armed one ends. 0 while
    /// disarmed.
    uint64_t epoch = 0;
    LaneArming arming = LaneArming::Disarmed; ///< Whether a frame on it is armed.
    LanePlace place = LanePlace::Idle;        ///< Who it waits on.
};

/// Test hook: what interrupt() calls between its two reads of a frame -- its serial, then its instance --
/// so that a test can replace the frame in between, as a lane thread racing it may.
struct LaneWatchProbe
{
    void (*betweenReads)(void* context) noexcept = nullptr; ///< Called, if set, with `context`.
    void* context = nullptr;                                ///< What it is called with.
};

/// One lane's frames as its thread runs them, for the backstop. Atomics only: the lane's thread records,
/// any thread interrupts, the supervisor observes, and none of them waits on another.
///
/// Every frame is given a serial unique on the lane, increasing as frames enter; an interrupt arms a frame by
/// raising the serial stored beside it to the frame's own, so that an arming outlives neither the frame nor a
/// frame entered later in its place, and an interrupter that read an older frame never lowers it.
class LaneWatch
{
  public:
    LaneWatch() noexcept = default;

    /// @param probe The test hook interrupt() calls between its reads.
    explicit LaneWatch(LaneWatchProbe probe) noexcept: _probe { probe } {}
    LaneWatch(LaneWatch const&) = delete;
    LaneWatch(LaneWatch&&) = delete;
    LaneWatch& operator=(LaneWatch const&) = delete;
    LaneWatch& operator=(LaneWatch&&) = delete;
    ~LaneWatch() = default;

    /// The lane's thread: a call into, or a memory read of, instance @p instanceId begins.
    /// @param instanceId The instance's id.
    void enter(uint32_t instanceId) noexcept;

    /// The lane's thread: the innermost frame ends, disarming it if it was armed.
    void leave() noexcept;

    /// The lane's thread: the innermost frame runs a host call, in the parent.
    void beginHostCall() noexcept;

    /// The lane's thread: the innermost frame's host call returned.
    void endHostCall() noexcept;

    /// Any thread, wait-free: arms the outermost frame of @p instanceId on the lane, if there is one; a frame
    /// armed already stays armed as it was. What it armed is what observe() sees.
    /// @param instanceId The instance interrupted.
    void interrupt(uint32_t instanceId) noexcept;

    /// The supervisor: what the lane does now. Wait-free; a frame that enters or leaves meanwhile may or may
    /// not be seen, as the next look will.
    [[nodiscard]] LaneObservation observe() const noexcept;

  private:
    /// Whether a frame runs on the lane's thread or waits on the parent.
    enum class FrameState : uint8_t
    {
        Running,     ///< It waits on the host.
        ServingHost, ///< It runs a host call.
    };

    /// One recorded frame.
    struct Frame
    {
        std::atomic<uint64_t> serial { 0 };      ///< Its serial; 0 until a frame was entered here.
        std::atomic<uint64_t> armedSerial { 0 }; ///< The serial an interrupt armed; armed iff it is `serial`.
        std::atomic<uint32_t> instanceId { 0 };  ///< The instance it runs.
        std::atomic<FrameState> state { FrameState::Running };
    };

    std::array<Frame, LaneFrameCapacity> _frames {};
    std::atomic<std::size_t> _depth { 0 };   ///< How many frames run, recorded or not.
    std::atomic<uint64_t> _nextSerial { 1 }; ///< The next frame's serial; the lane's thread's.
    LaneWatchProbe _probe {};                ///< Test hook; inert by default.
};

/// The lanes of one host, MaxLaneChannels of them.
class LaneWatchSet
{
  public:
    /// @param index The lane's index, below MaxLaneChannels.
    /// @return The lane.
    [[nodiscard]] LaneWatch& lane(std::size_t index) noexcept;

    /// Any thread, wait-free: arms the frames of @p instanceId on every lane (LaneWatch::interrupt()).
    /// @param instanceId The instance interrupted.
    void interrupt(uint32_t instanceId) noexcept;

    /// @return Every lane, in index order.
    [[nodiscard]] std::span<LaneWatch const> lanes() const noexcept { return _lanes; }

  private:
    std::array<LaneWatch, MaxLaneChannels> _lanes {};
};

} // namespace vtextension::wasm::remote
