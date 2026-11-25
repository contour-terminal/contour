// SPDX-License-Identifier: Apache-2.0
//
// The extension host executable itself, started as Contour starts it.
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace proto = vtextension::wasm::remote::proto;
namespace process = vtextension::wasm::remote::process;
using process::ExitKind;
using process::ExitStatus;
using process::testing::awaitReap;
using process::testing::HelperLaunch;
using process::testing::launchHelper;
using process::testing::ownProcessId;
using vtextension::wasm::remote::transport::PduStream;

namespace
{

/// The sanitizer variables a helper is handed when this process has them, so that it reports as the tests do.
constexpr auto SanitizerVariables =
    std::array<std::string_view, 4> { "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS" };

/// @return The helper's environment: the launcher's default, and this process's sanitizer variables.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> helperEnvironment()
{
    auto environment = process::defaultHelperEnvironment();
    auto const live = core::LiveEnvironment {};
    for (auto const name: SanitizerVariables)
        if (auto value = live.get(name))
            environment.emplace_back(std::string { name }, *std::move(value));
    return environment;
}

/// @return A launch of the extension host executable with @p leading arguments before its own.
[[nodiscard]] HelperLaunch helperLaunch(std::vector<std::string> leading = {},
                                        process::ProcessId parentProcessId = ownProcessId())
{
    return HelperLaunch { .executable = VTEXTENSION_HELPER_PATH,
                          .leading = std::move(leading),
                          .trailing = {},
                          .parentProcessId = parentProcessId,
                          .dataBytes = process::DefaultDataBytes,
                          .environment = helperEnvironment() };
}

#ifdef __linux__
/// A limit's soft and hard value, as /proc/<pid>/limits states them.
struct LimitValues
{
    std::string soft;
    std::string hard;

    bool operator==(LimitValues const&) const = default;
};

/// @return The soft and hard value of limit @p name (as `Max processes`) of process @p processId; nothing if
///         it is not listed.
[[nodiscard]] std::optional<LimitValues> limitOf(process::ProcessId processId, std::string_view name)
{
    auto limits = std::ifstream { std::format("/proc/{}/limits", processId) };
    auto line = std::string {};
    while (std::getline(limits, line))
    {
        if (!line.starts_with(name))
            continue;
        auto values = std::istringstream { line.substr(name.size()) };
        auto result = LimitValues {};
        values >> result.soft >> result.hard;
        return result;
    }
    return std::nullopt;
}
#endif

/// Receives the Hello on the parent end of @p helper's control channel, which must arrive.
/// @return The control stream, still open.
[[nodiscard]] PduStream receiveHello(process::testing::LaunchedHelper& helper)
{
    auto control = PduStream { std::move(helper.control),
                               proto::ChannelKind::Control,
                               proto::Direction::ToParent,
                               proto::PduLimits {} };
    auto const hello = control.receive();
    REQUIRE(hello.has_value());
    CHECK(*hello
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::Unsandboxed } });
    return control;
}

} // namespace

TEST_CASE("Helper.saysHelloOverInheritedChannels", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch());
    REQUIRE(helper.has_value());
    auto control = receiveHello(*helper);

    // Contour closes the connection: the helper exits cleanly, its sanitizers' reports included.
    {
        auto const closed = std::move(control);
    }
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 0 });
}

TEST_CASE("Helper.exitsOnBadArguments", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch({ "--bogus=1" }));
    REQUIRE(helper.has_value());
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 70 });
}

#ifndef _WIN32
TEST_CASE("Helper.exitsWhenItsParentIsAnother", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch({}, 1));
    REQUIRE(helper.has_value());
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 72 });
}
#endif

#ifdef __linux__
TEST_CASE("Helper.capsItselfBeforeHello", "[vtextension]")
{
    if constexpr (vtextension::wasm::host::BuildCapPolicy == vtextension::wasm::host::CapPolicy::Skip)
        SKIP("sanitizer builds apply no caps");
    constexpr auto DataBytes = uint64_t { 768 } * 1024 * 1024;
    auto launch = helperLaunch();
    launch.dataBytes = DataBytes;
    auto helper = launchHelper(process::makeSystemProcessLauncher(), std::move(launch));
    REQUIRE(helper.has_value());
    auto control = receiveHello(*helper);

    // Capped before Hello, soft and hard alike: no further thread, and the data bytes it was told of.
    auto const processId = helper->process->id();
    CHECK(limitOf(processId, "Max processes") == LimitValues { .soft = "0", .hard = "0" });
    CHECK(limitOf(processId, "Max data size")
          == LimitValues { .soft = std::to_string(DataBytes), .hard = std::to_string(DataBytes) });

    {
        auto const closed = std::move(control);
    }
    CHECK(awaitReap(*helper->launcher, *helper->process)
          == ExitStatus { .kind = ExitKind::Exited, .value = 0 });
}
#endif
