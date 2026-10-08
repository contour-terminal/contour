// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <expected>
#include <memory>
#include <optional>
#include <variant>

namespace
{

/// A PTY that refuses to start, as one does when the system has run out of them.
class UnstartablePty final: public vtpty::MockPty
{
  public:
    using vtpty::MockPty::MockPty;

    [[nodiscard]] vtpty::StartResult start() override
    {
        return std::unexpected(vtpty::StartFailure { .error = vtpty::StartError::PtyAllocationFailed,
                                                     .detail = "No PTY left." });
    }
};

/// @return A Process for a shell on @p pty, not yet started.
std::unique_ptr<vtpty::Process> shellOn(std::unique_ptr<vtpty::Pty> pty)
{
    return std::make_unique<vtpty::Process>(
        vtpty::Process::ExecInfo { .program = "/bin/sh", .arguments = {}, .workingDirectory = {}, .env = {} },
        std::move(pty),
        /*escapeSandbox=*/false);
}

auto constexpr PageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

} // namespace

TEST_CASE("Process.withoutChild", "[process]")
{
    // A Process has a child only once start() has spawned one. Before that -- and for good, when start()
    // failed before it got that far -- there is nobody to ask about, wait for or signal. It held pid 0
    // meanwhile, which POSIX reads as "the caller's whole process group": asking about it could reap an
    // unrelated child, terminating it hung up the terminal's own process group, and destroying it
    // dereferenced the empty exit status that waitpid() came back with.
    //
    // alive() is required to be false before terminate() is called, so that a regression fails here
    // rather than hanging up the process group the test runs in.

    SECTION("never started")
    {
        auto process = shellOn(std::make_unique<vtpty::MockPty>(PageSize));
        CHECK_FALSE(process->checkStatus().has_value());
        CHECK_FALSE(process->wait().has_value());
        REQUIRE_FALSE(process->alive());
        process->terminate(vtpty::Process::TerminationHint::Hangup);
#ifndef _WIN32
        // Nobody's directory to report -- and on a PTY that is not a UnixPty, nobody's to look up.
        CHECK(process->workingDirectory() == ".");
#endif
    }

    SECTION("start() failed before spawning")
    {
        auto process = shellOn(std::make_unique<UnstartablePty>(PageSize));
        REQUIRE_FALSE(process->start().has_value());
        CHECK_FALSE(process->checkStatus().has_value());
        CHECK_FALSE(process->wait().has_value());
        REQUIRE_FALSE(process->alive());
        process->terminate(vtpty::Process::TerminationHint::Hangup);
    }

    // Leaving either section destroys the Process, which has to return rather than wait on nobody.
}

#ifndef _WIN32
TEST_CASE("Process.withChild", "[process]")
{
    // The counterpart: once start() has spawned a child, it is that child -- and only that child -- which
    // is asked about, signalled and waited for.
    auto process = std::make_unique<vtpty::Process>(
        vtpty::Process::ExecInfo {
            .program = "/bin/sh", .arguments = { "-c", "exec sleep 30" }, .workingDirectory = {}, .env = {} },
        vtpty::createPty(PageSize, std::nullopt),
        /*escapeSandbox=*/false);
    REQUIRE(process->start().has_value());
    CHECK(process->alive());
    CHECK_FALSE(process->checkStatus().has_value());

    process->terminate(vtpty::Process::TerminationHint::Hangup);

    auto const status = process->wait();
    REQUIRE(status.has_value());
    auto const* const signalled = std::get_if<vtpty::Process::SignalExit>(&*status);
    REQUIRE(signalled != nullptr);
    CHECK(signalled->signum == SIGHUP);
    CHECK_FALSE(process->alive());
}
#endif
