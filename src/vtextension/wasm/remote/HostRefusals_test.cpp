// SPDX-License-Identifier: Apache-2.0
//
// Why no extension host serves, said once per reason.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::HostRefusalLog;
using vtextension::wasm::remote::LastHostRefusal;
using vtextension::wasm::remote::refusalLine;
using vtextension::wasm::remote::RefusalSource;
using vtextension::wasm::remote::refusalSource;
using vtextension::wasm::remote::unavailableStepLine;
using vtextension::wasm::remote::process::LaunchError;
using vtextension::wasm::remote::process::launchErrors;
using vtextension::wasm::remote::process::launchErrorText;

namespace proto = vtextension::wasm::remote::proto;

namespace
{

/// The lines a log wrote, from any thread.
struct Lines
{
    std::mutex mutex;
    std::vector<std::string> written;

    /// @return A writer appending to these lines.
    [[nodiscard]] HostRefusalLog::Writer writer()
    {
        return [this](std::string_view line) {
            auto const lock = std::scoped_lock { mutex };
            written.emplace_back(line);
        };
    }
};

/// @return @p text in lower case.
[[nodiscard]] std::string lowered(std::string_view text)
{
    auto result = std::string { text };
    for (auto& character: result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

} // namespace

TEST_CASE("HostRefusals.everyRefusalHasALine", "[vtextension]")
{
    for (auto const value: std::views::iota(0, std::to_underlying(LastHostRefusal) + 1))
    {
        auto const line = refusalLine(static_cast<HostRefusal>(value));
        INFO(line);
        CHECK_FALSE(line.empty());
        // A launch reason says what the launcher's own row says.
        if (refusalSource(static_cast<HostRefusal>(value)) == RefusalSource::Launch)
            CHECK(std::ranges::any_of(launchErrors(), [value](auto const& row) {
                return row.refusal == static_cast<HostRefusal>(value);
            }));
        // Users read these: they name the role, never the engine.
        for (auto const engine: { "wasm", "wamr", "wasmtime", "webassembly" })
            CHECK_FALSE(lowered(line).contains(engine));
    }
}

TEST_CASE("HostRefusals.launchLinesSayWhatTheLauncherSays", "[vtextension]")
{
    for (auto const& row: launchErrors())
    {
        INFO(row.text);
        CHECK(launchErrorText(row.error) == row.text);
        if (row.refusal)
            CHECK(
                refusalLine(*row.refusal)
                == std::format("The extension host cannot be started: {}. Extensions do not run.", row.text));
    }
    // Contour shutting down is no reason to tell of.
    CHECK_FALSE(launchErrors()[std::to_underlying(LaunchError::Stopped)].refusal.has_value());
}

TEST_CASE("HostRefusals.lockdownLineNamesTheStep", "[vtextension]")
{
    for (auto const& row: proto::lockdownSteps())
    {
        auto const line =
            refusalLine(HostRefusal::LockdownFailed, proto::LockdownFailure { .step = row.step, .code = 0 });
        INFO(line);
        if (row.step == proto::LockdownStep::None)
        {
            // A host that names no step is refused in the general words.
            CHECK(line == refusalLine(HostRefusal::LockdownFailed));
            continue;
        }
        CHECK(line.contains(row.text));
        for (auto const engine: { "wasm", "wamr", "wasmtime", "webassembly" })
            CHECK_FALSE(lowered(line).contains(engine));
    }
    // Any other reason has no step to name.
    CHECK(refusalLine(HostRefusal::Unsandboxed,
                      proto::LockdownFailure { .step = proto::LockdownStep::SystemCalls, .code = 1 })
          == refusalLine(HostRefusal::Unsandboxed));

    // What the step's code says follows the step: the system's word on an errno, read on this side, as a
    // release build's host has nowhere else to say it.
    auto const withCode =
        refusalLine(HostRefusal::LockdownFailed,
                    proto::LockdownFailure { .step = proto::LockdownStep::SystemCalls, .code = 1 });
    CHECK(withCode.contains(std::string { proto::lockdownStepText(proto::LockdownStep::SystemCalls) } + " ("
                            + proto::systemErrorText(1) + ")"));

    // The log names the step the first refusal for the reason named.
    auto lines = Lines {};
    auto log = HostRefusalLog { lines.writer() };
    auto const fileSystem = proto::LockdownFailure { .step = proto::LockdownStep::FileSystem, .code = 13 };
    log.refused(HostRefusal::LockdownFailed, fileSystem);
    log.refused(HostRefusal::LockdownFailed,
                proto::LockdownFailure { .step = proto::LockdownStep::SystemCalls, .code = 1 });
    CHECK(lines.written == std::vector { refusalLine(HostRefusal::LockdownFailed, fileSystem) });
}

TEST_CASE("HostRefusals.unavailableStepIsLoggedOnce", "[vtextension]")
{
    auto lines = Lines {};
    auto log = HostRefusalLog { lines.writer() };
    log.unavailable(proto::LockdownStep::None);
    log.unavailable(proto::LockdownStep::FileSystem);
    log.unavailable(proto::LockdownStep::FileSystem);
    CHECK(lines.written == std::vector { unavailableStepLine(proto::LockdownStep::FileSystem) });
    auto const& line = lines.written.front();
    CHECK(line.contains(proto::lockdownStepText(proto::LockdownStep::FileSystem)));
    for (auto const engine: { "wasm", "wamr", "wasmtime", "webassembly" })
        CHECK_FALSE(lowered(line).contains(engine));
    // A value naming no step logs nothing.
    log.unavailable(static_cast<proto::LockdownStep>(200));
    CHECK(lines.written.size() == 1);
}

TEST_CASE("HostRefusals.eachReasonIsLoggedOnce", "[vtextension]")
{
    auto lines = Lines {};
    auto log = HostRefusalLog { lines.writer() };
    log.refused(HostRefusal::LockdownFailed);
    log.refused(HostRefusal::LockdownFailed);
    log.refused(HostRefusal::HelperNotFound);
    log.refused(HostRefusal::LockdownFailed);
    log.refused(HostRefusal::HelperNotFound);
    CHECK(lines.written
          == std::vector<std::string> {
              refusalLine(HostRefusal::LockdownFailed),
              refusalLine(HostRefusal::HelperNotFound),
          });

    // Another log -- another backend's -- says it again.
    auto other = HostRefusalLog { lines.writer() };
    other.refused(HostRefusal::LockdownFailed);
    CHECK(lines.written.size() == 3);
}

TEST_CASE("HostRefusals.oneLineHoweverManyThreadsRefuse", "[vtextension][threading]")
{
    constexpr auto Threads = 8;
    auto lines = Lines {};
    auto log = HostRefusalLog { lines.writer() };
    {
        auto threads = std::vector<std::jthread> {};
        for ([[maybe_unused]] auto const index: std::views::iota(0, Threads))
            threads.emplace_back([&log] {
                for ([[maybe_unused]] auto const attempt: std::views::iota(0, 100))
                    log.refused(HostRefusal::Unsandboxed);
            });
    }
    CHECK(lines.written.size() == 1);
}
