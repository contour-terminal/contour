// SPDX-License-Identifier: Apache-2.0
//
// How long a guest takes to parse the largest reply of a command it runs, under the bundled interpreter:
// the measurement behind the agent orchestrator's 50 ms question. The guest is the orchestrator's parse
// harness (extensions/orchestrator/harness), a raw WASI reactor exporting `reserve` and `parse`; it is
// loaded on the engine the extension host creates, with the host's default limits, and `parse` is timed
// over a recorded reply.
//
// Hidden (`[.measure]`) and driven from outside, since neither file is built or kept by this tree: export
//
//     ORCH_PARSE_MODULE=<orchestrator_parse_harness.wasm>
//     ORCH_PARSE_FIXTURE=<extensions/orchestrator/core/testdata/github/prs-100.json>
//
// and run `vtextension_test "[.measure]"`.
//
// With either variable unset the case skips.
#include <core/Environment.hpp>
#include <core/Utils.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/Engine.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace
{

using vtextension::ExtensionError;
using vtextension::ExtensionLimits;
using vtextension::wasm::IWasmInstance;
using Clock = std::chrono::steady_clock;

/// How many times `parse` is timed per run; the first call also grows the guest's heap.
constexpr auto Parses = std::size_t { 5 };

/// How many pull requests the largest `gh-prs` reply holds: the command's `--limit`.
constexpr auto LargestReplyEntries = uint32_t { 100 };

/// The harness's `parse` result for a reply it refused.
constexpr auto Refused = uint32_t { 0xFFFF'FFFF };

/// What the harness's host context answers: the harness imports WASI only, never a host function.
class NoHost final: public vtextension::wasm::IHostContext
{
  public:
    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        return std::unexpected(ExtensionError::Trapped);
    }
};

/// @return @p text as a module's bytes.
[[nodiscard]] std::vector<std::byte> toBytes(std::string const& text)
{
    auto bytes = std::vector<std::byte>(text.size());
    std::ranges::transform(text, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

/// Calls the export @p name of @p instance, which must exist and return.
/// @param argv Its parameters on entry, its results on return.
void call(IWasmInstance& instance, std::string_view name, std::span<uint32_t> argv)
{
    INFO("calling " << name);
    auto const function = instance.function(name);
    REQUIRE(function.has_value());
    REQUIRE(instance.call(*function, argv).has_value());
}

/// @return @p duration in fractional milliseconds.
[[nodiscard]] double toMilliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

TEST_CASE("ParseBudget: the orchestrator parses its largest gh reply", "[.measure]")
{
    auto const& environment = core::defaultEnvironment();
    auto const modulePath = environment.get("ORCH_PARSE_MODULE");
    auto const fixturePath = environment.get("ORCH_PARSE_FIXTURE");
    if (!modulePath || !fixturePath)
        SKIP("set ORCH_PARSE_MODULE and ORCH_PARSE_FIXTURE to measure");

    auto const moduleBytes = core::readFileAsString(*modulePath);
    auto const reply = core::readFileAsString(*fixturePath);
    REQUIRE_FALSE(moduleBytes.empty());
    REQUIRE_FALSE(reply.empty());

    // The engine the extension host creates, with the host's limits: memory and stack caps included.
    auto const limits = ExtensionLimits {};
    auto const backend = vtextension::wasm::makeDefaultBackend();
    auto engine = vtextension::Engine::create(*backend, limits);
    REQUIRE(engine.has_value());
    auto const guard = backend->enterThread();
    REQUIRE(guard.has_value());
    auto const module = (*engine)->runtime().load(toBytes(moduleBytes));
    REQUIRE(module.has_value());
    auto host = NoHost {};
    auto const instance = (*module)->instantiate(host);
    REQUIRE(instance.has_value());
    auto& guest = **instance;

    auto none = std::array<uint32_t, 0> {};
    call(guest, "_initialize", none);

    auto reserved = std::array { static_cast<uint32_t>(reply.size()) };
    call(guest, "reserve", reserved);
    auto const address = reserved[0];
    REQUIRE(address != 0);
    auto const buffer = guest.memory(address, static_cast<uint32_t>(reply.size()));
    REQUIRE(buffer.has_value());
    std::ranges::transform(reply, buffer->begin(), [](char c) { return static_cast<std::byte>(c); });

    auto durations = std::vector<Clock::duration> {};
    while (durations.size() < Parses)
    {
        auto argv = std::array { address, static_cast<uint32_t>(reply.size()) };
        auto const start = Clock::now();
        call(guest, "parse", argv);
        durations.push_back(Clock::now() - start);
        REQUIRE(argv[0] != Refused);
        CHECK(argv[0] == LargestReplyEntries);
    }

    auto line = std::format("parse {} bytes:", reply.size());
    for (auto const duration: durations)
        line += std::format(" {:.2f}", toMilliseconds(duration));
    auto sorted = durations;
    std::ranges::sort(sorted);
    std::cout << std::format("{} ms; first {:.2f} ms, median {:.2f} ms, max {:.2f} ms (deadline {} ms)\n",
                             line,
                             toMilliseconds(durations.front()),
                             toMilliseconds(sorted[sorted.size() / 2]),
                             toMilliseconds(sorted.back()),
                             limits.callDeadline.count());
}
