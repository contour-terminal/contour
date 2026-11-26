// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>

#include <core/Clock.hpp>
#include <core/Environment.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/testing/Wine.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <unistd.h>
#endif

namespace vtextension::wasm::remote::process::testing
{

using transport::Channel;
using transport::NativeHandle;

namespace
{
    /// The sanitizer variables a helper is handed when this process has them. Leak detection is off in a
    /// sandboxed helper by its own defaults (host/SanitizerOptions.cpp) unless they turn it back on.
    constexpr auto SanitizerVariables =
        std::array<std::string_view, 4> { "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS" };
} // namespace

std::vector<std::pair<std::string, std::string>> sanitizerEnvironment()
{
    auto environment = std::vector<std::pair<std::string, std::string>> {};
    auto const live = core::LiveEnvironment {};
    for (auto const name: SanitizerVariables)
        if (auto value = live.get(name))
            environment.emplace_back(std::string { name }, *std::move(value));
    return environment;
}

ProcessIsolation helperIsolation() noexcept
{
#ifdef _WIN32
    return wasm::testing::runsOnWine() ? ProcessIsolation::None : ProcessIsolation::AppContainer;
#else
    return ProcessIsolation::None;
#endif
}

ProcessId ownProcessId() noexcept
{
    return currentProcessId();
}

std::optional<transport::NativeHandle> ownStandardError() noexcept
{
#ifdef _WIN32
    if (auto* const own = ::GetStdHandle(STD_ERROR_HANDLE); own != nullptr && own != INVALID_HANDLE_VALUE)
        return own;
    return std::nullopt;
#else
    return STDERR_FILENO;
#endif
}

#ifdef _WIN32
std::optional<CapturedStandardError> CapturedStandardError::create()
{
    auto path = std::wstring(MAX_PATH, L'\0');
    auto const length = ::GetTempPathW(static_cast<DWORD>(path.size()), path.data());
    if (length == 0 || length >= path.size())
        return std::nullopt;
    path.resize(length);
    auto name = std::wstring(MAX_PATH, L'\0');
    if (::GetTempFileNameW(path.c_str(), L"ceh", 0, name.data()) == 0)
        return std::nullopt;
    auto* const file = ::CreateFileW(name.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr,
                                     CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                     nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::nullopt;
    return CapturedStandardError { file };
}

CapturedStandardError::CapturedStandardError(CapturedStandardError&& other) noexcept:
    _file { std::exchange(other._file, INVALID_HANDLE_VALUE) }
{
}

CapturedStandardError::~CapturedStandardError()
{
    if (_file != INVALID_HANDLE_VALUE)
        ::CloseHandle(_file);
}

std::string CapturedStandardError::text() const
{
    // The child's copy shares this handle's file position: read from the start, then leave it at the end.
    auto text = std::string {};
    if (!::SetFilePointerEx(_file, LARGE_INTEGER {}, nullptr, FILE_BEGIN))
        return text;
    auto buffer = std::array<char, 4096> {};
    auto read = DWORD { 0 };
    while (::ReadFile(_file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read != 0)
        text.append(buffer.data(), read);
    return text;
}
#endif

namespace
{
    /// @return How @p status says a process ended: its exit code in hex, or its signal.
    [[nodiscard]] std::string endingOf(std::optional<ExitStatus> const& status)
    {
        if (!status)
            return "it still runs, or could not be waited for";
        return std::format("it ended: {}", exitStatusText(*status));
    }

    /// @return @p ending, then what the process wrote: @p written.
    [[nodiscard]] std::string withStandardError(std::string const& ending, std::string const& written)
    {
        if (written.empty())
            return std::format("{}; it wrote nothing to stderr", ending);
        return std::format("{}; its stderr: {}", ending, written);
    }
} // namespace

std::string endOf(IProcessLauncher& launcher,
                  IProcessHandle& process,
                  StandardErrorReader const& standardError)
{
    // Reaped first: what it wrote as it ended is in the file by then.
    auto const ending = endingOf(awaitReap(launcher, process));
    return withStandardError(ending, standardError());
}

void checkEndedCleanly(IProcessLauncher& launcher,
                       IProcessHandle& process,
                       StandardErrorReader const& standardError)
{
    auto const status = awaitReap(launcher, process);
    auto const clean = ExitStatus { .kind = ExitKind::Exited, .value = 0 };
    if (status != clean)
        UNSCOPED_INFO(withStandardError(endingOf(status), standardError()));
    CHECK(status == clean);
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
        auto const status = process.tryReap();
        if (!status)
            return std::nullopt;
        if (*status)
            return *status;
        if (clock.now() >= deadline)
            return std::nullopt;
        if (!(*set)->wait(deadline))
            return std::nullopt;
    }
}

std::optional<std::vector<std::string>> readReport(transport::Channel& channel)
{
    auto text = std::string {};
    auto buffer = std::array<uint8_t, 4096> {};
    while (!text.ends_with("end\n"))
    {
        auto const received = channel.receiveSome(buffer);
        if (!received)
            return std::nullopt;
        text.append(reinterpret_cast<char const*>(buffer.data()), *received);
    }
    auto lines = std::vector<std::string> {};
    for (auto const line: std::views::split(std::string_view { text }, '\n'))
        if (!line.empty())
            lines.emplace_back(std::string_view { line });
    return lines;
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

PreparedHelperLaunch prepareHelperLaunch(HelperLaunch launch)
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
                             .caps = {},
                             .standardError = launch.standardError,
                             .isolation = launch.isolation };
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
    return PreparedHelperLaunch { .spec = std::move(spec),
                                  .control = *std::move(control),
                                  .lanes = std::move(lanes),
                                  .page = *std::move(page),
                                  .wake = *std::move(wake),
                                  .childHandles = std::move(childHandles) };
}

std::expected<LaunchedHelper, LaunchError> launchHelper(std::shared_ptr<IProcessLauncher> launcher,
                                                        HelperLaunch launch)
{
    auto prepared = prepareHelperLaunch(std::move(launch));
    auto process = launcher->launch(prepared.spec);
    if (!process)
        return std::unexpected(process.error());
    // The helper's ends are the child's alone now: the parent's copies close here, as a connector's do when
    // its spec goes out of scope.
    auto parentLanes = std::vector<Channel> {};
    for (auto& lane: prepared.lanes)
        parentLanes.push_back(std::move(lane.parent));
    return LaunchedHelper { std::move(launcher),
                            *std::move(process),
                            std::move(prepared.control.parent),
                            std::move(parentLanes),
                            std::move(prepared.page),
                            std::move(prepared.wake.parent),
                            std::move(prepared.childHandles) };
}

} // namespace vtextension::wasm::remote::process::testing
