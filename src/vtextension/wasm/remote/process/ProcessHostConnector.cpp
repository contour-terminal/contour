// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/ProcessHostConnector.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

namespace vtextension::wasm::remote::process
{

using transport::NativeHandle;

namespace
{
    /// Debug notes about how extension host processes are connected; off unless asked for.
    auto const connectionLog = core::log::Category("vtextension.host.connection",
                                                   "Logs how extension host processes are connected.");

    /// Notes once, at debug level, that this system discards rather than refuses descriptors a host sends
    /// up: nothing is installed either way, but a discarded one is closed on a thread of Contour's.
    void noteUpwardDescriptors(transport::UpwardDescriptors upward)
    {
        if (upward == transport::UpwardDescriptors::Refused)
            return;
        static auto noted = std::once_flag {};
        std::call_once(noted, [] {
            connectionLog()("This system cannot make Contour's ends refuse descriptors an extension host "
                            "sends; it discards them instead.");
        });
    }

    /// A host process, as its parent holds it: the supervisor's grip on it, and the parent's ends of its
    /// interrupt page and wake signal.
    class ProcessHostHandle final: public remote::IHostHandle
    {
      public:
        ProcessHostHandle(SupervisedHelper helper,
                          transport::ControlPage page,
                          transport::WakeSender wake) noexcept:
            _page { std::move(page) }, _wake { std::move(wake) }, _helper { std::move(helper) }
        {
        }

        /// Raises the slot, wakes the host's listener to look at the page, and wakes the supervisor to look
        /// at the lanes, which the caller armed. A full wake signal means a wake is pending already.
        void interrupt(uint32_t interruptSlot) noexcept override
        {
            _page.raise(interruptSlot);
            std::ignore = _wake.signal();
            _helper.nudge();
        }

        void clearInterrupt(uint32_t interruptSlot) noexcept override { _page.clear(interruptSlot); }

        void terminate() noexcept override { _helper.kill(); }
        void letGo() noexcept override { _helper.letGo(); }

        [[nodiscard]] remote::StepWatch watch(remote::HostStep step,
                                              std::chrono::milliseconds budget) override
        {
            return _helper.watch(step, budget);
        }

        void attach(remote::HostAttachment attachment) override { _helper.attach(std::move(attachment)); }

        [[nodiscard]] transport::HandleTarget const& handleTarget() const noexcept override
        {
            return _helper.handleTarget();
        }

      private:
        transport::ControlPage _page;
        transport::WakeSender _wake;
        SupervisedHelper _helper; ///< Last, so that it goes first: let go before its page and wake signal.
    };
} // namespace

ProcessHostConnector::ProcessHostConnector(std::shared_ptr<SupervisorMailbox> supervisor,
                                           ProcessHostConfig config,
                                           remote::HostRefusalLog::Writer refusals):
    _supervisor { std::move(supervisor) }, _config { std::move(config) }, _refusals { std::move(refusals) }
{
}

void ProcessHostConnector::refused(remote::HostRefusal refusal)
{
    if (remote::refusalPersistence(refusal) == remote::RefusalPersistence::Lasting)
        _refusalHistory.store(RefusalHistory::Lasting);
}

std::expected<remote::HostConnection, ExtensionError> ProcessHostConnector::connect()
{
    // Said once already, by whoever was told the host's Hello: no host is started that would fail alike.
    if (_refusalHistory.load() == RefusalHistory::Lasting)
        return std::unexpected(ExtensionError::HostUnavailable);
    auto control = transport::makeChannelPair();
    auto page = transport::ControlPage::create();
    auto wake = transport::makeWakePair();
    if (!control || !page || !wake)
        return std::unexpected(ExtensionError::HostUnavailable);
    auto lanes = std::vector<transport::ChannelPair> {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, MaxLaneChannels))
    {
        auto lane = transport::makeChannelPair();
        if (!lane)
            return std::unexpected(ExtensionError::HostUnavailable);
        lanes.push_back(*std::move(lane));
    }
    noteUpwardDescriptors(control->upward);

    // The child sees them in this order (HelperArguments): control, the lanes, the page, the wake signal.
    auto spec = LaunchSpec { .executable = _config.helper,
                             .inherited = {},
                             .arguments = {},
                             .environment = _config.environment,
                             .caps = _config.caps,
                             .standardError = _config.standardError,
                             .isolation = _config.isolation };
    spec.inherited.push_back(control->helper.nativeHandle());
    for (auto const& lane: lanes)
        spec.inherited.push_back(lane.helper.nativeHandle());
    spec.inherited.push_back(page->nativeHandle());
    spec.inherited.push_back(wake->helper.nativeHandle());
    spec.arguments = [extra = _config.extraArguments,
                      dataBytes = _config.caps.dataBytes](std::span<NativeHandle const> handles) {
        auto arguments = transport::HelperArguments { .control = handles[0],
                                                      .lanes = {},
                                                      .controlPage = handles[1 + MaxLaneChannels],
                                                      .wake = handles[2 + MaxLaneChannels],
                                                      .parentProcessId = currentProcessId(),
                                                      .dataBytes = dataBytes };
        std::ranges::copy(handles.subspan(1, MaxLaneChannels), arguments.lanes.begin());
        auto formatted = transport::formatHelperArguments(arguments);
        std::ranges::copy(extra, std::back_inserter(formatted));
        return formatted;
    };

    auto helper = _supervisor->launch(std::move(spec));
    if (!helper)
    {
        if (auto const refusal = launchErrors()[std::to_underlying(helper.error())].refusal)
            _refusals.refused(*refusal);
        return std::unexpected(ExtensionError::HostUnavailable);
    }
    // The helper's ends are the child's alone now: the parent's copies close as they go out of scope.
    // The memory objects of its instances are held to the data cap it is started with.
    auto connection = remote::HostConnection { .control = std::move(control->parent),
                                               .lanes = {},
                                               .handle = {},
                                               .objectBytesCap = _config.caps.dataBytes };
    for (auto& lane: lanes)
        connection.lanes.push_back(std::move(lane.parent));
    connection.handle =
        std::make_unique<ProcessHostHandle>(*std::move(helper), *std::move(page), std::move(wake->parent));
    return connection;
}

} // namespace vtextension::wasm::remote::process
