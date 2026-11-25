// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>

#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <tuple>

#include <vtextension/wasm/remote/transport/HelperArguments.hpp>

namespace vtextension::wasm::remote::process::testing
{

using transport::Channel;
using transport::NativeHandle;

ProcessId ownProcessId() noexcept
{
    return currentProcessId();
}

std::optional<ExitStatus> awaitReap(IProcessLauncher& launcher, IProcessHandle& process)
{
    auto clock = core::SteadyClock {};
    auto const deadline = clock.now() + ChildPatience;
    auto set = launcher.makeWaitSet(clock);
    if (!set || !(*set)->watch(0, process))
        return std::nullopt;
    while (true)
    {
        if (auto const status = process.tryReap())
            return status;
        if (clock.now() >= deadline)
            return std::nullopt;
        if (!(*set)->wait(deadline))
            return std::nullopt;
    }
}

bool endAndReap(IProcessLauncher& launcher, IProcessHandle& process)
{
    process.kill();
    return awaitReap(launcher, process).has_value();
}

LaunchedHelper::LaunchedHelper(std::shared_ptr<IProcessLauncher> launcher,
                               std::unique_ptr<IProcessHandle> process,
                               Channel control,
                               std::vector<Channel> lanes,
                               transport::ControlPage page,
                               transport::WakeSender wake,
                               std::shared_ptr<std::vector<NativeHandle>> childHandles) noexcept:
    launcher { std::move(launcher) },
    process { std::move(process) },
    control { std::move(control) },
    lanes { std::move(lanes) },
    page { std::move(page) },
    wake { std::move(wake) },
    childHandles { std::move(childHandles) }
{
}

LaunchedHelper::~LaunchedHelper()
{
    if (process)
        std::ignore = endAndReap(*launcher, *process);
}

void LaunchedHelper::closeControl() noexcept
{
    control.shutdown();
    auto const closed = std::move(control);
}

std::expected<LaunchedHelper, LaunchError> launchHelper(std::shared_ptr<IProcessLauncher> launcher,
                                                        HelperLaunch launch)
{
    auto control = transport::makeChannelPair();
    REQUIRE(control.has_value());
    auto lanes = std::vector<transport::ChannelPair> {};
    for ([[maybe_unused]] auto const i: std::views::iota(std::size_t { 0 }, MaxLaneChannels))
    {
        auto lane = transport::makeChannelPair();
        REQUIRE(lane.has_value());
        lanes.push_back(*std::move(lane));
    }
    auto page = transport::ControlPage::create();
    REQUIRE(page.has_value());
    auto wake = transport::makeWakePair();
    REQUIRE(wake.has_value());

    auto spec = LaunchSpec { .executable = std::move(launch.executable),
                             .inherited = {},
                             .arguments = {},
                             .environment = std::move(launch.environment),
                             .caps = {} };
    spec.inherited.push_back(control->helper.nativeHandle());
    for (auto const& lane: lanes)
        spec.inherited.push_back(lane.helper.nativeHandle());
    spec.inherited.push_back(page->nativeHandle());
    spec.inherited.push_back(wake->helper.nativeHandle());

    auto childHandles = std::make_shared<std::vector<NativeHandle>>();
    spec.arguments = [childHandles,
                      leading = std::move(launch.leading),
                      trailing = std::move(launch.trailing),
                      parentProcessId = launch.parentProcessId,
                      dataBytes = launch.dataBytes](std::span<NativeHandle const> handles) {
        childHandles->assign(handles.begin(), handles.end());
        auto arguments = transport::HelperArguments { .control = handles[0],
                                                      .lanes = {},
                                                      .controlPage = handles[1 + MaxLaneChannels],
                                                      .wake = handles[2 + MaxLaneChannels],
                                                      .parentProcessId = parentProcessId,
                                                      .dataBytes = dataBytes };
        std::ranges::copy(handles.subspan(1, MaxLaneChannels), arguments.lanes.begin());
        auto result = leading;
        std::ranges::copy(transport::formatHelperArguments(arguments), std::back_inserter(result));
        if (trailing)
            std::ranges::copy(trailing(handles), std::back_inserter(result));
        return result;
    };

    auto process = launcher->launch(spec);
    if (!process)
        return std::unexpected(process.error());
    // The helper's ends are the child's alone now: the parent's copies close here, as a connector's do when
    // its spec goes out of scope.
    auto parentLanes = std::vector<Channel> {};
    for (auto& lane: lanes)
        parentLanes.push_back(std::move(lane.parent));
    return LaunchedHelper { std::move(launcher),    *std::move(process), std::move(control->parent),
                            std::move(parentLanes), *std::move(page),    std::move(wake->parent),
                            std::move(childHandles) };
}

} // namespace vtextension::wasm::remote::process::testing
