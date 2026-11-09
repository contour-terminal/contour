// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/SwitchGuard.hpp>

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using contour::remote::DaemonAddress;
using contour::remote::SwitchGrants;
using contour::session::PermissionDecision;
using contour::session::PermissionMemory;
using contour::session::SwitchRemembering;
using contour::session::SwitchTarget;
using vthost::proto::SwitchDaemonKind;
using vthost::proto::SwitchOutcome;

namespace
{

[[nodiscard]] DaemonAddress label(std::string name)
{
    return DaemonAddress { .kind = SwitchDaemonKind::Label, .name = std::move(name) };
}

[[nodiscard]] SwitchTarget toLabel(std::string name, std::string_view workspace = {})
{
    return SwitchTarget { .kind = SwitchDaemonKind::Label,
                          .daemon = std::move(name),
                          .workspace = contour::session::workspaceTargetOf(workspace) };
}

[[nodiscard]] SwitchTarget toHost(std::string name)
{
    return SwitchTarget { .kind = SwitchDaemonKind::Host, .daemon = std::move(name), .workspace = {} };
}

/// A WindowSwitching that records what it was asked, and answers each request with @c outcome.
class RecordingSwitching final: public contour::session::WindowSwitching
{
  public:
    struct Request
    {
        vtworkspace::WorkspaceId window;
        SwitchTarget target;
        contour::session::SwitchMode mode {};
        contour::session::SwitchLandBy landBy; ///< When it must have landed by.
    };

    void request(vtworkspace::WorkspaceId window,
                 SwitchTarget target,
                 contour::session::SwitchMode mode,
                 contour::session::SwitchLandBy landBy,
                 contour::session::SwitchCompletion done) override
    {
        requests.push_back({ .window = window, .target = std::move(target), .mode = mode, .landBy = landBy });
        done(outcome);
    }

    std::vector<Request> requests;                                               ///< Every request, in order.
    std::expected<SwitchOutcome, std::string> outcome = SwitchOutcome::Switched; ///< What each is told.
};

/// The guard's world: the grants, the switching, the questions asked and what the switch was told.
struct GuardRig
{
    SwitchGrants grants;
    RecordingSwitching switching;
    std::vector<contour::session::SwitchAsk> asked;
    std::optional<std::string> askFailure; ///< When set, asking fails with this reason.
    std::optional<std::pair<SwitchOutcome, std::string>> told;
    std::vector<std::function<void()>> expiries;   ///< The questions' expiries, armed and not fired.
    std::vector<std::chrono::milliseconds> delays; ///< Each expiry's delay, in arming order.
    int withdrawn = 0;                             ///< How many questions were taken down unanswered.
    core::platform::ManualClock clock { core::platform::SteadyTimePoint {} + std::chrono::hours { 1 } };
    contour::remote::SwitchDeadline deadline {
        .timeout = contour::remote::DefaultSwitchTimeout,
        .timer =
            [this](std::chrono::milliseconds delay, std::function<void()> fire) {
                delays.push_back(delay);
                expiries.push_back(std::move(fire));
            },
        .clock = clock,
    };

    /// Runs a switch of window 7, from @p source to @p target, through the guard.
    /// @param answerWithin How long the daemon says it waits for the reply.
    void run(DaemonAddress source,
             SwitchTarget target,
             std::chrono::milliseconds answerWithin = std::chrono::milliseconds {
                 vthost::SwitchAnswerTimeout })
    {
        told.reset();
        contour::remote::runGuardedSwitch(
            contour::remote::GuardedSwitch { .window = vtworkspace::WorkspaceId { 7 },
                                             .source = std::move(source),
                                             .target = std::move(target),
                                             .answerWithin = answerWithin },
            grants,
            [this](vtworkspace::WorkspaceId /*window*/, contour::session::SwitchAsk ask)
                -> std::expected<contour::remote::SwitchWithdraw, std::string> {
                if (askFailure)
                    return std::unexpected(*askFailure);
                asked.push_back(std::move(ask));
                return contour::remote::SwitchWithdraw { [this] { ++withdrawn; } };
            },
            switching,
            deadline,
            [this](SwitchOutcome outcome, std::string detail) { told.emplace(outcome, std::move(detail)); });
    }

    /// Answers the question asked last.
    void answer(PermissionDecision decision, PermissionMemory memory)
    {
        REQUIRE_FALSE(asked.empty());
        asked.back().answered(decision, memory);
    }
};

} // namespace

TEST_CASE("SwitchGrants: an answer stands for its pair of daemons and no other", "[switch][guard]")
{
    struct Row
    {
        DaemonAddress source;
        SwitchTarget target;
        std::optional<PermissionDecision> remembered;
    };

    auto grants = SwitchGrants {};
    grants.remember(label("default"), toLabel("admin"), PermissionDecision::Allow);
    grants.remember(label("work"),
                    SwitchTarget { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} },
                    PermissionDecision::Deny);
    grants.remember(label("default"), toHost("home"), PermissionDecision::Deny);

    for (auto const& row: {
             // The pair answered, whichever workspace it names.
             Row { label("default"), toLabel("admin"), PermissionDecision::Allow },
             Row { label("default"), toLabel("admin", "build"), PermissionDecision::Allow },
             // Another target, or another source, is asked again.
             Row { label("default"), toLabel("work"), std::nullopt },
             Row { label("work"), toLabel("admin"), std::nullopt },
             // The window's own daemon, named or not, is one key.
             Row { label("work"),
                   SwitchTarget { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} },
                   PermissionDecision::Deny },
             Row { label("work"), toLabel("work"), PermissionDecision::Deny },
             Row { label("default"),
                   SwitchTarget { .kind = SwitchDaemonKind::Same, .daemon = {}, .workspace = {} },
                   std::nullopt },
             // A saved host is a daemon like any other; a label of the same name is not it.
             Row { label("default"), toHost("home"), PermissionDecision::Deny },
             Row { label("default"), toLabel("home"), std::nullopt },
         })
    {
        INFO(std::format(
            "{} -> {}:{}", row.source.name, std::to_underlying(row.target.kind), row.target.daemon));
        CHECK(grants.remembered(row.source, row.target) == row.remembered);
    }
}

TEST_CASE("SwitchGrants: an ad-hoc host is never remembered", "[switch][guard]")
{
    auto grants = SwitchGrants {};
    for (auto const decision: { PermissionDecision::Allow, PermissionDecision::Deny })
    {
        grants.remember(label("default"), toHost("ssh://x"), decision);
        CHECK_FALSE(grants.remembered(label("default"), toHost("ssh://x")).has_value());
    }
    CHECK(contour::remote::rememberingFor(toHost("ssh://me@box:22")) == SwitchRemembering::Withheld);
    CHECK(contour::remote::rememberingFor(toHost("home")) == SwitchRemembering::Offered);
    CHECK(contour::remote::rememberingFor(toLabel("ssh")) == SwitchRemembering::Offered);
}

TEST_CASE("runGuardedSwitch: a remembered Deny refuses without asking", "[switch][guard]")
{
    auto rig = GuardRig {};
    rig.grants.remember(label("default"), toLabel("admin"), PermissionDecision::Deny);

    rig.run(label("default"), toLabel("admin", "build"));

    CHECK(rig.asked.empty());
    CHECK(rig.switching.requests.empty());
    REQUIRE(rig.told.has_value());
    CHECK(rig.told->first == SwitchOutcome::Refused);
    CHECK(rig.told->second == contour::remote::SwitchRefusedDetail);
}

TEST_CASE("runGuardedSwitch: the user is asked, and an answer asked to stand is kept", "[switch][guard]")
{
    auto rig = GuardRig {};

    rig.run(label("default"), toLabel("admin", "build"));
    REQUIRE(rig.asked.size() == 1);
    CHECK(rig.asked.front().target == toLabel("admin", "build"));
    CHECK(rig.asked.front().sourceDaemon == "default");
    CHECK(rig.asked.front().remembering == SwitchRemembering::Offered);
    CHECK_FALSE(rig.told.has_value());

    rig.answer(PermissionDecision::Allow, PermissionMemory::Session);
    REQUIRE(rig.switching.requests.size() == 1);
    CHECK(rig.switching.requests.front().window == vtworkspace::WorkspaceId { 7 });
    CHECK(rig.switching.requests.front().mode == contour::session::SwitchMode::Switch);
    CHECK(rig.told == std::pair { SwitchOutcome::Switched, std::string {} });
    CHECK(rig.grants.remembered(label("default"), toLabel("admin")) == PermissionDecision::Allow);

    // Now the pair switches without asking; a failed switch says why.
    rig.switching.outcome = std::unexpected(std::string { "no such workspace" });
    rig.run(label("default"), toLabel("admin", "other"));
    CHECK(rig.asked.size() == 1);
    CHECK(rig.told == std::pair { SwitchOutcome::Failed, std::string { "no such workspace" } });

    // An answer once is not kept.
    rig.run(label("default"), toLabel("work"));
    REQUIRE(rig.asked.size() == 2);
    rig.answer(PermissionDecision::Deny, PermissionMemory::Once);
    CHECK(rig.told
          == std::pair { SwitchOutcome::Refused, std::string { contour::remote::SwitchRefusedDetail } });
    CHECK_FALSE(rig.grants.remembered(label("default"), toLabel("work")).has_value());
    // A second answer to one question is not a second switch.
    rig.answer(PermissionDecision::Allow, PermissionMemory::Once);
    CHECK(rig.switching.requests.size() == 2);
}

TEST_CASE("runGuardedSwitch: an ad-hoc host always asks, offering nothing to remember", "[switch][guard]")
{
    auto rig = GuardRig {};

    rig.run(label("default"), toHost("ssh://x"));
    REQUIRE(rig.asked.size() == 1);
    CHECK(rig.asked.front().remembering == SwitchRemembering::Withheld);
    rig.answer(PermissionDecision::Allow, PermissionMemory::Session);
    CHECK(rig.told == std::pair { SwitchOutcome::Switched, std::string {} });

    rig.run(label("default"), toHost("ssh://x"));
    CHECK(rig.asked.size() == 2);
}

TEST_CASE("runGuardedSwitch: a window with nothing to ask in fails the switch", "[switch][guard]")
{
    auto rig = GuardRig {};
    rig.askFailure = "the window has no pane to ask in";

    rig.run(label("default"), toLabel("admin"));

    CHECK(rig.switching.requests.empty());
    CHECK(rig.told
          == std::pair { SwitchOutcome::Failed, std::string { "the window has no pane to ask in" } });
}

// Review (PR 3, item 8): the daemon stops waiting for the answer after vthost::SwitchAnswerTimeout.
TEST_CASE("runGuardedSwitch refuses a question left unanswered, and takes it down", "[switch][guard]")
{
    auto rig = GuardRig {};
    rig.run(label("default"), toLabel("admin"));
    REQUIRE(rig.asked.size() == 1);
    REQUIRE(rig.expiries.size() == 1);
    CHECK(rig.delays.front()
          == contour::remote::switchQuestionExpiry(std::chrono::milliseconds { vthost::SwitchAnswerTimeout },
                                                   contour::remote::DefaultSwitchTimeout));
    CHECK_FALSE(rig.told.has_value());

    rig.expiries.front()();

    REQUIRE(rig.told.has_value());
    CHECK(rig.told->first == SwitchOutcome::Refused);
    CHECK(rig.told->second == contour::remote::SwitchUnansweredDetail);
    CHECK(rig.withdrawn == 1);
    // An answer after it switches nothing.
    rig.answer(PermissionDecision::Allow, PermissionMemory::Once);
    CHECK(rig.switching.requests.empty());

    SECTION("an answer before it stands, and the expiry finds nothing to do")
    {
        rig.run(label("default"), toLabel("admin"));
        rig.answer(PermissionDecision::Allow, PermissionMemory::Once);
        REQUIRE(rig.told.has_value());
        CHECK(rig.told->first == SwitchOutcome::Switched);
        rig.expiries.back()();
        CHECK(rig.told->first == SwitchOutcome::Switched);
        CHECK(rig.withdrawn == 1);
    }
}

TEST_CASE(
    "runGuardedSwitch takes the question down while an Allow can still land before the daemon's deadline",
    "[switch][guard]")
{
    using namespace std::chrono_literals;
    // The deadline the daemon carries, less the switch's own landing time and the reply's margin.
    STATIC_CHECK(contour::remote::switchQuestionExpiry(30s, contour::remote::DefaultSwitchTimeout)
                 == 30s - contour::remote::DefaultSwitchTimeout - contour::remote::SwitchReplyMargin);
    auto rig = GuardRig {};
    rig.run(label("default"), toLabel("admin"), 20s);
    REQUIRE(rig.asked.size() == 1);
    REQUIRE(rig.delays.size() == 1);
    CHECK(rig.delays.front()
          == contour::remote::switchQuestionExpiry(20s, contour::remote::DefaultSwitchTimeout));
    // An Allow arriving after the last moment it could land is refused: the question is gone.
    rig.expiries.front()();
    rig.answer(PermissionDecision::Allow, PermissionMemory::Once);
    REQUIRE(rig.told.has_value());
    CHECK(rig.told->first == SwitchOutcome::Refused);
    CHECK(rig.switching.requests.empty());
}

TEST_CASE("runGuardedSwitch refuses at once a switch the daemon would stop waiting for before it could land",
          "[switch][guard]")
{
    using namespace std::chrono_literals;
    auto rig = GuardRig {};
    auto const tooShort = contour::remote::DefaultSwitchTimeout + contour::remote::SwitchReplyMargin;

    SECTION("asked: nothing is asked")
    {
        rig.run(label("default"), toLabel("admin"), tooShort);
    }
    SECTION("allowed to stand: nothing switches")
    {
        rig.grants.remember(label("default"), toLabel("admin"), PermissionDecision::Allow);
        rig.run(label("default"), toLabel("admin"), tooShort);
    }
    REQUIRE(rig.told.has_value());
    CHECK(rig.told->first == SwitchOutcome::Refused);
    CHECK(rig.told->second == contour::remote::SwitchTooLateDetail);
    CHECK(rig.asked.empty());
    CHECK(rig.expiries.empty());
    CHECK(rig.switching.requests.empty());
}

// Review (PR 4, I1): the question's expiry left the switch its own 10 s, but the switch lists the target
// daemon's workspaces first, so an Allow near the expiry could land after the daemon gave up. The switch now
// carries when it must land by, counted from the request's arrival, and the landing bound is the switcher's.
TEST_CASE("runGuardedSwitch has an allowed switch land before the daemon gives up, counted from its arrival",
          "[switch][guard][timeout]")
{
    using namespace std::chrono_literals;
    auto rig = GuardRig {};
    rig.deadline.timeout = 4s;
    auto const arrival = rig.clock.now();
    rig.run(label("default"), toLabel("admin"), 20s);
    // The landing bound is the one the switcher is built with, not the default.
    REQUIRE(rig.delays.size() == 1);
    CHECK(rig.delays.front() == 20s - 4s - contour::remote::SwitchReplyMargin);

    // Answered late, the switch still lands by the time counted from the arrival.
    rig.clock.advance(9s);
    rig.answer(PermissionDecision::Allow, PermissionMemory::Once);

    REQUIRE(rig.switching.requests.size() == 1);
    CHECK(rig.switching.requests.front().landBy == arrival + 20s - contour::remote::SwitchReplyMargin);
}

// Review (PR 4, M7): the wait a daemon states was taken as it came, up to 49 days from another host.
TEST_CASE("the wait a daemon states for a switch's answer is bounded", "[switch][guard][timeout]")
{
    using namespace std::chrono_literals;
    STATIC_CHECK(contour::remote::switchAnswerWindowOf(0)
                 == std::chrono::milliseconds { vthost::SwitchAnswerTimeout });
    STATIC_CHECK(contour::remote::switchAnswerWindowOf(20'000) == 20s);
    CHECK(contour::remote::switchAnswerWindowOf(std::numeric_limits<uint32_t>::max())
          == contour::remote::MaxSwitchAnswerWindow);
    STATIC_CHECK(contour::remote::MaxSwitchAnswerWindow == 5min);
}
