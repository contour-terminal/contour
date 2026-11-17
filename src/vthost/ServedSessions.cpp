// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServedSessions.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <vthost/Logging.hpp>

namespace vthost
{

/// What one device's sinks record. Shared by the sinks, which may outlive the ServedSessions that made them.
struct ServedSessions::DeviceState
{
    /// Whether the input is dropping what does not fit: one episode is logged once.
    enum class InputOverflow : std::uint8_t
    {
        None = 0, ///< The last chunk fit.
        Dropping, ///< The last chunk was dropped.
    };

    explicit DeviceState(vtbackend::PageSize initial): size { initial } {}

    /// Appends @p chunk to the input, whole, if it fits; drops it whole otherwise.
    /// @param chunk One write of the terminal's.
    void accept(std::string_view chunk)
    {
        if (chunk.size() > MaxServedInputBytes)
        {
            sessionLog()("a served tab's input of {} bytes can never fit its bound of {} bytes; dropped",
                         chunk.size(),
                         MaxServedInputBytes);
            return;
        }
        auto const lock = std::lock_guard { mutex };
        if (input.size() + chunk.size() > MaxServedInputBytes)
        {
            if (std::exchange(overflow, InputOverflow::Dropping) == InputOverflow::None)
                sessionLog()("a served tab's input is full; whole chunks are dropped until it is read");
            return;
        }
        overflow = InputOverflow::None;
        input.append(chunk);
    }

    mutable std::mutex mutex; ///< Guards everything below.
    std::string input;
    vtbackend::PageSize size;
    InputOverflow overflow = InputOverflow::None;
};

ServedSessions::ServedSessions(std::shared_ptr<vtcontrol::UuidSource> ids): _ids { std::move(ids) }
{
}

std::shared_ptr<vtpty::ChannelPty> ServedSessions::makeDevice(vtbackend::PageSize size)
{
    auto device = std::make_shared<vtpty::ChannelPty>(size);
    auto state = std::make_shared<DeviceState>(size);
    device->setWriteSink(
        [state](std::string_view chunk, vtpty::InputKind /*kind*/) { state->accept(chunk); });
    device->setResizeSink([state](vtbackend::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
        auto const lock = std::lock_guard { state->mutex };
        state->size = cells;
    });
    auto const lock = std::lock_guard { _mutex };
    // The entries of devices nobody holds any more go as a new one comes.
    std::erase_if(_devices, [](Entry const& entry) { return entry.device.expired(); });
    _devices.push_back(Entry { .device = device, .state = std::move(state) });
    return device;
}

std::string ServedSessions::inputOf(vtpty::ChannelPty const& device) const
{
    auto state = std::shared_ptr<DeviceState> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find_if(
            _devices, [&](Entry const& entry) { return entry.device.lock().get() == &device; });
        if (found == _devices.end())
            return {};
        state = found->state;
    }
    auto const lock = std::lock_guard { state->mutex };
    return state->input;
}

} // namespace vthost
