// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/Hosting.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using contour::cli::AttachResult;
using contour::cli::attemptsAttach;
using contour::cli::decideHosting;
using contour::cli::HostingDecision;
using contour::cli::hostingNotice;
using contour::config::DaemonMode;

TEST_CASE("decideHosting covers every mode and outcome", "[cli][hosting]")
{
    using enum contour::config::DaemonMode;
    static_assert(!attemptsAttach(InProcess) && attemptsAttach(Auto) && attemptsAttach(Required));
    static_assert(decideHosting(InProcess, std::nullopt) == HostingDecision::InProcess);
    static_assert(decideHosting(Auto, AttachResult::Attached) == HostingDecision::Attached);
    static_assert(decideHosting(Auto, AttachResult::Unreachable) == HostingDecision::InProcessWithNotice);
    static_assert(decideHosting(Auto, AttachResult::Refused) == HostingDecision::InProcessWithNotice);
    static_assert(decideHosting(Required, AttachResult::Refused) == HostingDecision::Fail);
    static_assert(decideHosting(Required, AttachResult::Attached) == HostingDecision::Attached);
    // The rows a launch reaches less often: an attach never made, and a failure under `required`.
    static_assert(decideHosting(Required, AttachResult::Unreachable) == HostingDecision::Fail);
    static_assert(decideHosting(Auto, std::nullopt) == HostingDecision::InProcessWithNotice);
    SUCCEED();
}

TEST_CASE("a launch naming a program is never attached", "[cli][hosting]")
{
    using enum contour::config::DaemonMode;
    using contour::cli::LaunchProgram;
    static_assert(!attemptsAttach(InProcess, LaunchProgram::Named));
    static_assert(!attemptsAttach(Auto, LaunchProgram::Named));
    static_assert(!attemptsAttach(Required, LaunchProgram::Named));
    static_assert(attemptsAttach(Auto, LaunchProgram::Profile)
                  && attemptsAttach(Required, LaunchProgram::Profile));
    // Every row's named-program column, whatever an attach would have said.
    static_assert(decideHosting(InProcess, LaunchProgram::Named, std::nullopt) == HostingDecision::InProcess);
    static_assert(decideHosting(Auto, LaunchProgram::Named, std::nullopt) == HostingDecision::InProcess);
    static_assert(decideHosting(Auto, LaunchProgram::Named, AttachResult::Attached)
                  == HostingDecision::InProcess);
    static_assert(decideHosting(Required, LaunchProgram::Named, std::nullopt) == HostingDecision::Fail);
    // The profile's shell keeps the rows above.
    static_assert(decideHosting(Required, LaunchProgram::Profile, AttachResult::Attached)
                  == HostingDecision::Attached);
    static_assert(decideHosting(Auto, LaunchProgram::Profile, AttachResult::Unreachable)
                  == HostingDecision::InProcessWithNotice);
    CHECK(contour::cli::NamedProgramRefusal
          == "daemon.mode is required, but a program given on the command line cannot be attached to the "
             "daemon; drop it or use --daemon-mode=in_process");
}

TEST_CASE("the notice names the reason", "[cli][hosting]")
{
    CHECK(hostingNotice("the daemon speaks protocol 6-8, this client speaks 4-4")
          == "Contour could not reach its daemon (the daemon speaks protocol 6-8, this client speaks 4-4). "
             "This window runs its shells itself; they end when it closes.");
}

TEST_CASE("a launch's daemon mode: forced, else the option, else the configuration", "[cli][hosting]")
{
    using contour::cli::resolveDaemonMode;
    CHECK(resolveDaemonMode(DaemonMode::Required, "in_process", DaemonMode::Auto) == DaemonMode::Required);
    CHECK(resolveDaemonMode(std::nullopt, "auto", DaemonMode::InProcess) == DaemonMode::Auto);
    CHECK(resolveDaemonMode(std::nullopt, "", DaemonMode::Required) == DaemonMode::Required);
    auto const unknown = resolveDaemonMode(std::nullopt, "sometimes", DaemonMode::InProcess);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error()
          == "unknown --daemon-mode 'sometimes' (expected one of: in_process, auto, required)");
}

TEST_CASE("the daemon's socket and size policy: the option, else the configuration", "[cli][hosting]")
{
    using contour::cli::resolveSizePolicy;
    using contour::cli::resolveSocketOption;
    CHECK(resolveSizePolicy("", vthost::ClientSizePolicy::Smallest) == vthost::ClientSizePolicy::Smallest);
    CHECK(resolveSizePolicy("largest", vthost::ClientSizePolicy::Smallest)
          == vthost::ClientSizePolicy::Largest);
    CHECK_FALSE(resolveSizePolicy("biggest", vthost::ClientSizePolicy::Latest).has_value());
    CHECK(resolveSocketOption("", "/run/contour/configured") == "/run/contour/configured");
    CHECK(resolveSocketOption("/tmp/given", "/run/contour/configured") == "/tmp/given");
}
