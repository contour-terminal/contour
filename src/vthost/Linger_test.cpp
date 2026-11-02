// SPDX-License-Identifier: Apache-2.0
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/Linger.hpp>
#include <vthost/testing/FakeCommandRunner.hpp>

using vthost::LingerState;

namespace
{
LingerState stateFor(vthost::CommandOutcome outcome)
{
    auto runner = vthost::testing::FakeCommandRunner {};
    runner.answerAll(std::move(outcome));
    return vthost::lingerStateOf(runner, "alice");
}
} // namespace

TEST_CASE("lingerStateOf reads Linger=yes and Linger=no", "[vthost][linger]")
{
    CHECK(stateFor({ 0, "Linger=yes\n" }) == LingerState::On);
    CHECK(stateFor({ 0, "Linger=no\n" }) == LingerState::Off);
}

TEST_CASE("lingerStateOf is Unknown for anything else", "[vthost][linger]")
{
    CHECK(stateFor({ 0, "something odd\n" }) == LingerState::Unknown);
    CHECK(stateFor({ 1, "Linger=no\n" }) == LingerState::Unknown);

    auto runner = vthost::testing::FakeCommandRunner {};
    runner.failToStart(std::make_error_code(std::errc::no_such_file_or_directory));
    CHECK(vthost::lingerStateOf(runner, "alice") == LingerState::Unknown);
}

TEST_CASE("lingerStateOf asks loginctl about the named user", "[vthost][linger]")
{
    auto runner = vthost::testing::FakeCommandRunner {};
    runner.answerAll({ 0, "Linger=no\n" });
    std::ignore = vthost::lingerStateOf(runner, "alice");
    REQUIRE(runner.calls.size() == 1);
    CHECK(runner.calls[0] == std::vector<std::string> { "loginctl", "show-user", "alice", "-p", "Linger" });
}

TEST_CASE("lingerStateOf runs nothing without a user", "[vthost][linger]")
{
    auto runner = vthost::testing::FakeCommandRunner {};
    CHECK(vthost::lingerStateOf(runner, "") == LingerState::Unknown);
    CHECK(runner.calls.empty());
}

namespace
{
std::string_view hintFor(vthost::ServiceStartMode mode,
                         vthost::testing::FakeCommandRunner& runner,
                         std::optional<std::string> user = "alice")
{
    auto const env =
        user ? core::testing::FakeEnvironment { { { "USER", *user } } } : core::testing::FakeEnvironment {};
    return vthost::lingerHintAfterInstall(mode, runner, env);
}
} // namespace

TEST_CASE("lingerHintAfterInstall speaks only for Logon with lingering off", "[vthost][linger]")
{
    using vthost::ServiceStartMode;
    auto runner = vthost::testing::FakeCommandRunner {};

    runner.answerAll({ 0, "Linger=no\n" });
    CHECK(hintFor(ServiceStartMode::Logon, runner) == vthost::lingerAdvice());
    CHECK(vthost::lingerAdvice().contains("loginctl enable-linger"));

    runner.answerAll({ 0, "Linger=yes\n" });
    CHECK(hintFor(ServiceStartMode::Logon, runner).empty());

    runner.failToStart(std::make_error_code(std::errc::no_such_file_or_directory));
    CHECK(hintFor(ServiceStartMode::Logon, runner).empty());
    runner.answerAll({ 1, "Linger=no\n" });
    CHECK(hintFor(ServiceStartMode::Logon, runner).empty());
}

TEST_CASE("lingerHintAfterInstall runs nothing for other modes or without USER", "[vthost][linger]")
{
    using vthost::ServiceStartMode;
    auto runner = vthost::testing::FakeCommandRunner {};
    runner.answerAll({ 0, "Linger=no\n" });

    CHECK(hintFor(ServiceStartMode::Boot, runner).empty());
    CHECK(hintFor(ServiceStartMode::Manual, runner).empty());
    CHECK(hintFor(ServiceStartMode::Logon, runner, std::nullopt).empty());
    CHECK(runner.calls.empty());
}
