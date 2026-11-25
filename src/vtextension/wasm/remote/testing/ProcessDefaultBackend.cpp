// SPDX-License-Identifier: Apache-2.0
//
// The default backend of the extension host process's test binary: the remote backend, over real helper
// processes the binary's one supervisor starts. Compiled into vtextension_helper_test only, so that the
// contract and the suites written against the default backend run their real guests through a process
// boundary, unchanged -- and, through a listener, fail when a helper of theirs ended uncleanly: exited
// non-zero, or died of a signal the supervisor did not send.
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

#include <vtextension/testing/RunGate.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>
#include <vtextension/wasm/testing/PreparedGeneration.hpp>

namespace vtextension::wasm
{

std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config)
{
    // One kind and one supervisor for the binary, as a program composes them.
    auto const& composition = remote::process::testing::binaryComposition();
    // No connector configured: this composition starts the real helper, locked down where the build has a
    // sandbox -- so that every suite here runs in it.
    if (!config.connector)
    {
        config.connector = composition.connector(remote::process::testing::HelperBackendConfig {
            .helper = VTEXTENSION_HELPER_PATH,
            .extraArguments = {},
            .lockdown = remote::process::testing::HelperLockdown,
            .timeouts = config.timeouts });
        config.lockdown = remote::process::testing::HelperLockdown;
    }
    return remote::makeRemoteBackend(composition.kind(), std::move(config));
}

} // namespace vtextension::wasm

namespace vtextension::wasm::testing
{

uint64_t preparedGeneration() noexcept
{
    return remote::preparedGeneration();
}

} // namespace vtextension::wasm::testing

namespace
{

/// The clean-exit check for every case of the binary: once a case ended, waits until the helpers it let go
/// were reaped, and fails the run should any helper of the binary's composition have ended uncleanly
/// meanwhile -- how a helper's sanitizer report fails the case that caused it. A case's helpers exit once its
/// backends are gone, which is by the time it ended.
class UncleanHelperExits final: public Catch::EventListenerBase
{
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const& /*info*/) override
    {
        auto const& supervisor =
            vtextension::wasm::remote::process::testing::binaryComposition().supervisor();
        _liveBefore = supervisor.liveProcesses();
        _uncleanBefore = supervisor.counters().uncleanExits;
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override
    {
        auto const& composition = vtextension::wasm::remote::process::testing::binaryComposition();
        if (!composition.awaitLiveHelpersAtMost(_liveBefore))
            vtextension::testing::runGateFailures().push_back(
                std::format("{}: an extension host it started outlived it", stats.testInfo->name));
        auto const unclean = composition.supervisor().counters().uncleanExits - _uncleanBefore;
        if (unclean != 0)
            vtextension::testing::runGateFailures().push_back(
                std::format("{}: {} extension host(s) ended uncleanly -- see their report above",
                            stats.testInfo->name,
                            unclean));
    }

  private:
    std::size_t _liveBefore = 0;
    std::size_t _uncleanBefore = 0;
};

} // namespace

CATCH_REGISTER_LISTENER(UncleanHelperExits)
