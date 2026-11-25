// SPDX-License-Identifier: Apache-2.0
//
// Why no extension host serves, said once per reason.
#include <catch2/catch_test_macros.hpp>

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

using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::HostRefusalLog;
using vtextension::wasm::remote::refusalLine;
using vtextension::wasm::remote::process::LaunchError;
using vtextension::wasm::remote::process::launchErrors;
using vtextension::wasm::remote::process::launchErrorText;

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
    for (auto const value: std::views::iota(0, std::to_underlying(HostRefusal::MemoryMode) + 1))
    {
        auto const line = refusalLine(static_cast<HostRefusal>(value));
        INFO(line);
        CHECK_FALSE(line.empty());
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
