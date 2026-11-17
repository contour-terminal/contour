// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ServedSessions` -- the devices of served sessions: hosted sessions that run no program, whose screen a
/// daemon-scoped extension draws through a host-owned vtpty::ChannelPty, and whose keyboard and mouse input
/// the host buffers for it to read.

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/ChannelPty.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <vtcontrol/Uuid.hpp>

namespace vthost
{

/// The most input one served tab buffers for its extension: what one read of it takes. A chunk the terminal
/// writes that does not fit whole is dropped whole, never cut, so that its reader never sees half a sequence.
inline constexpr std::size_t MaxServedInputBytes = std::size_t { 64 } * 1024;

/// The served sessions of one SessionHost: makes the device each one runs on, and holds what each one's
/// terminal wrote until its extension reads it. Thread-safe: a device is written to on its session's pump
/// thread, and read from wherever the extension runs.
class ServedSessions final
{
  public:
    /// @param ids Where the UUIDs of served tabs come from.
    explicit ServedSessions(std::shared_ptr<vtcontrol::UuidSource> ids);

    ServedSessions(ServedSessions const&) = delete;
    ServedSessions(ServedSessions&&) = delete;
    ServedSessions& operator=(ServedSessions const&) = delete;
    ServedSessions& operator=(ServedSessions&&) = delete;
    ~ServedSessions() = default;

    /// Makes the device of a served tab: what is fed to it is its screen's output; what its terminal writes
    /// is appended to its input in whole chunks while they fit (@see MaxServedInputBytes); a resize is
    /// recorded. Any thread.
    /// @param size The size the device starts at.
    /// @return The device, shared with the session that adopts it.
    [[nodiscard]] std::shared_ptr<vtpty::ChannelPty> makeDevice(vtbackend::PageSize size);

    /// @param device A device makeDevice() made.
    /// @return What its terminal wrote and nobody read yet; empty for a device this did not make. Any thread.
    [[nodiscard]] std::string inputOf(vtpty::ChannelPty const& device) const;

  private:
    struct DeviceState;

    /// One device, as long as anyone holds it, and what it buffered.
    struct Entry
    {
        std::weak_ptr<vtpty::ChannelPty const> device;
        std::shared_ptr<DeviceState> state;
    };

    std::shared_ptr<vtcontrol::UuidSource> _ids; ///< Where the UUIDs of served tabs come from.
    mutable std::mutex _mutex;                   ///< Guards _devices.
    std::vector<Entry> _devices;
};

} // namespace vthost
