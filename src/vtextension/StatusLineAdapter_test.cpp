// SPDX-License-Identifier: Apache-2.0
//
// The terminal's view of a pane's extensions: vtbackend's events and kinds mapped onto the world's, and
// the queries forwarded to the ExtensionHost.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/ExtensionHost.hpp>
#include <vtextension/StatusLineAdapter.hpp>
#include <vtextension/testing/RunnerHarness.hpp>
#include <vtextension/testing/TestModules.hpp>

using namespace vtextension;
using vtextension::testing::RunnerHarness;
using vtextension::testing::toModule;

namespace
{

[[nodiscard]] ExtensionSpec spec(std::string name, Placement placement, std::vector<std::byte> module)
{
    return ExtensionSpec { .name = std::move(name),
                           .hostId = "adapter@test",
                           .module = std::move(module),
                           .config = {},
                           .placement = placement,
                           .runCommand = CommandPermission::Deny };
}

[[nodiscard]] std::vector<std::byte> unloadable()
{
    return { std::byte { 0 }, std::byte { 1 } };
}

} // namespace

TEST_CASE("StatusLineAdapter.toEvent", "[vtextension]")
{
    namespace from = vtbackend::StatusLineEvents;
    using vtbackend::StatusLineEvent;

    struct Case
    {
        StatusLineEvent from;
        Event to;
    };
    auto const cases = std::vector<Case> {
        { from::TitleChanged { .title = "t" }, vtextension::TitleChanged { .title = "t" } },
        { from::CwdChanged { .cwd = "/x" }, vtextension::CwdChanged { .cwd = "/x" } },
        { from::ForegroundProcessChanged { .name = "vim" },
          vtextension::ForegroundProcessChanged { .name = "vim" } },
        { from::InputModeChanged { .mode = vtbackend::ViMode::Insert },
          vtextension::InputModeChanged { .mode = InputMode::Insert } },
        { from::InputModeChanged { .mode = vtbackend::ViMode::Normal },
          vtextension::InputModeChanged { .mode = InputMode::Normal } },
        { from::InputModeChanged { .mode = vtbackend::ViMode::Visual },
          vtextension::InputModeChanged { .mode = InputMode::Visual } },
        { from::InputModeChanged { .mode = vtbackend::ViMode::VisualLine },
          vtextension::InputModeChanged { .mode = InputMode::VisualLine } },
        { from::InputModeChanged { .mode = vtbackend::ViMode::VisualBlock },
          vtextension::InputModeChanged { .mode = InputMode::VisualBlock } },
        { from::InputModeChanged { .mode = vtbackend::ViMode::Hint },
          vtextension::InputModeChanged { .mode = InputMode::Hint } },
        { from::TabsChanged {
              .tabs = { { .title = "a", .activity = vtbackend::StatusLineTabActivity::Inactive },
                        { .title = "b", .activity = vtbackend::StatusLineTabActivity::Active } } },
          vtextension::TabsChanged { .tabs = { { .title = "a", .activity = TabActivity::Inactive },
                                               { .title = "b", .activity = TabActivity::Active } } } },
        { from::UserVarChanged { .name = "k", .value = "v" },
          vtextension::UserVarChanged { .name = "k", .value = "v" } },
        { from::ProgressChanged {
              .progress = { .state = vtbackend::ProgressState::Inactive, .percentage = 0 } },
          vtextension::ProgressChanged { .progress = { .state = ProgressState::Hidden, .percent = 0 } } },
        { from::ProgressChanged {
              .progress = { .state = vtbackend::ProgressState::Normal, .percentage = 42 } },
          vtextension::ProgressChanged { .progress = { .state = ProgressState::Normal, .percent = 42 } } },
        { from::ProgressChanged { .progress = { .state = vtbackend::ProgressState::Error, .percentage = 7 } },
          vtextension::ProgressChanged { .progress = { .state = ProgressState::Error, .percent = 7 } } },
        { from::ProgressChanged {
              .progress = { .state = vtbackend::ProgressState::Indeterminate, .percentage = 0 } },
          vtextension::ProgressChanged {
              .progress = { .state = ProgressState::Indeterminate, .percent = 0 } } },
        { from::ProgressChanged {
              .progress = { .state = vtbackend::ProgressState::Paused, .percentage = 50 } },
          vtextension::ProgressChanged { .progress = { .state = ProgressState::Warning, .percent = 50 } } },
        { from::FocusChanged { .state = vtbackend::StatusLineFocus::Focused },
          vtextension::FocusChanged { .state = FocusState::Focused } },
        { from::FocusChanged { .state = vtbackend::StatusLineFocus::Unfocused },
          vtextension::FocusChanged { .state = FocusState::Unfocused } },
        { from::FontChanged {
              .metrics = { .pointSize = 11.5F,
                           .cellPixelSize =
                               vtbackend::ImageSize { vtbackend::Width(9), vtbackend::Height(19) } } },
          vtextension::FontChanged {
              .metrics = FontMetrics { .pointSize = 11.5F, .cellWidth = 9, .cellHeight = 19 } } },
    };

    for (auto const& [from, to]: cases)
    {
        INFO("alternative " << from.index());
        auto const event = toEvent(from);
        CHECK(event == to);
        // Each kind is its world's kind, bit for bit.
        CHECK(std::to_underlying(kindOf(event)) == std::to_underlying(vtbackend::kindOf(from)));
    }
}

TEST_CASE("StatusLineAdapter.kinds", "[vtextension]")
{
    // The terminal's kinds are the subscribable ones; the always-delivered ones are the host's business.
    CHECK(toStatusLineEventKinds(AlwaysDelivered).none());
    CHECK(toStatusLineEventKinds(SubscribableKinds).value() == SubscribableKinds.value());
    CHECK(toStatusLineEventKinds(EventKinds { EventKind::Cwd, EventKind::Timer })
          == vtbackend::StatusLineEventKinds { vtbackend::StatusLineEventKind::Cwd });
}

TEST_CASE("StatusLineAdapter.forwards", "[vtextension]")
{
    auto harness = RunnerHarness {};
    auto engines = EngineProvider { ExtensionLimits {} };
    auto config = ExtensionHostConfig {};
    config.extensions.push_back(
        spec("git", Placement::Segment, toModule(vtextension::testing::MinimalModule)));
    config.extensions.push_back(spec("plasma", Placement::TakeOver, unloadable()));
    auto host =
        ExtensionHost(std::move(config),
                      ExtensionHostServices { .engines = engines,
                                              .runners = harness.services(harness.scheduler, engines) });
    auto adapter = StatusLineAdapter { host };

    CHECK(adapter.takeOverName() == "plasma");
    REQUIRE(adapter.snapshot("plasma") != nullptr);
    CHECK(adapter.snapshot("plasma") == host.snapshot("plasma"));
    CHECK(adapter.snapshot("nothing") == nullptr);

    adapter.resize(
        vtbackend::PageSize { .lines = vtbackend::LineCount(1), .columns = vtbackend::ColumnCount(80) });
    adapter.post(vtbackend::StatusLineEvents::CwdChanged { .cwd = "/" });
    harness.scheduler.runPending();
    REQUIRE(adapter.snapshot("git") != nullptr);
    CHECK(adapter.snapshot("git") == host.snapshot("git"));
    // The minimal module subscribes to nothing; what it receives regardless is not the terminal's to know.
    CHECK(host.subscriptions() == AlwaysDelivered);
    CHECK(adapter.subscriptions().none());

    SECTION("without a take-over extension there is none")
    {
        auto segmentsOnly = ExtensionHostConfig {};
        segmentsOnly.extensions.push_back(spec("broken", Placement::Segment, unloadable()));
        auto other =
            ExtensionHost(std::move(segmentsOnly),
                          ExtensionHostServices { .engines = engines,
                                                  .runners = harness.services(harness.scheduler, engines) });
        CHECK_FALSE(StatusLineAdapter { other }.takeOverName().has_value());
    }
}

TEST_CASE("StatusLineAdapter.toEvent.invalidUtf8", "[vtextension]")
{
    // The world's strings are UTF-8, and a program may set anything: every byte that does not decode
    // becomes U+FFFD, in every string an event carries.
    namespace from = vtbackend::StatusLineEvents;
    auto const ff = std::string { "\xFF\xFF" };
    auto const truncated = std::string { "ab\xE2\x82" }; // the first two bytes of U+20AC
    auto const replaced = std::string { "��" };

    CHECK(toEvent(from::UserVarChanged { .name = ff, .value = ff })
          == Event { vtextension::UserVarChanged { .name = replaced, .value = replaced } });
    CHECK(toEvent(from::TitleChanged { .title = truncated })
          == Event { vtextension::TitleChanged { .title = "ab" + replaced } });
    CHECK(toEvent(from::CwdChanged { .cwd = ff }) == Event { vtextension::CwdChanged { .cwd = replaced } });
    CHECK(toEvent(from::ForegroundProcessChanged { .name = ff })
          == Event { vtextension::ForegroundProcessChanged { .name = replaced } });
    CHECK(toEvent(from::TabsChanged { .tabs = { { .title = ff } } })
          == Event { vtextension::TabsChanged { .tabs = { { .title = replaced } } } });
    // Valid text, multi-byte included, is kept as it is.
    CHECK(toEvent(from::TitleChanged { .title = "€ \U0001F600" })
          == Event { vtextension::TitleChanged { .title = "€ \U0001F600" } });
}

TEST_CASE("StatusLineAdapter.publisher", "[vtextension]")
{
    // The host's runners tell of their frames through the terminal's revocable sink, never a raw handle.
    struct CountingTarget final: vtbackend::IStatusLinePublishTarget
    {
        std::atomic<int> delivered = 0;
        void statusLineExtensionsPublished() noexcept override { ++delivered; }
    };
    auto sink = vtbackend::StatusLinePublishSink {};
    auto target = CountingTarget {};
    sink.attach(target);

    auto harness = RunnerHarness {};
    auto engines = EngineProvider { ExtensionLimits {} };
    auto runners = harness.services(harness.scheduler, engines);
    runners.onSnapshotPublished = publisherFor(sink);
    auto config = ExtensionHostConfig {};
    config.extensions.push_back(
        spec("git", Placement::Segment, toModule(vtextension::testing::MinimalModule)));
    auto host =
        ExtensionHost(std::move(config), ExtensionHostServices { .engines = engines, .runners = runners });
    host.resize(
        vtbackend::PageSize { .lines = vtbackend::LineCount(1), .columns = vtbackend::ColumnCount(80) });
    harness.scheduler.runPending();
    CHECK(target.delivered > 0);

    sink.revoke();
    auto const delivered = target.delivered.load();
    host.resize(
        vtbackend::PageSize { .lines = vtbackend::LineCount(1), .columns = vtbackend::ColumnCount(60) });
    harness.scheduler.runPending();
    CHECK(target.delivered == delivered);
}
