// SPDX-License-Identifier: Apache-2.0
//
// The extension host executable itself, started as Contour starts it.
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

} // namespace

TEST_CASE("Helper.saysHelloOverInheritedChannels", "[vtextension]")
{
    auto helper = launchHelper(process::makeSystemProcessLauncher(), helperLaunch());
    REQUIRE(helper.has_value());
    auto control = PduStream { std::move(helper->control),
                               proto::ChannelKind::Control,
                               proto::Direction::ToParent,
                               proto::PduLimits {} };
    auto const hello = control.receive();
    REQUIRE(hello.has_value());
    CHECK(*hello
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::Unsandboxed } });

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
