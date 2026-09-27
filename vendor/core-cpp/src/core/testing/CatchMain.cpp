// SPDX-License-Identifier: Apache-2.0
//
// The main() of every core-cpp test binary (core::testing_main). It applies the LOG filter, runs
// Catch2, and returns the exit status that core::testing::normalisedExitCode() maps the run to.

#include <core/Environment.hpp>
#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>
#include <core/testing/ExitCode.hpp>
#include <core/testing/SuppressWindowsDialogs.hpp>

#include <catch2/catch_session.hpp>
#include <catch2/interfaces/catch_interfaces_reporter.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace
{

// What the last test run counted. Catch::Session::run() returns only an exit status, so the
// listener below keeps the totals the run ended with.
Catch::Totals lastTotals {};

class TotalsListener final: public Catch::EventListenerBase
{
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(Catch::TestRunStats const& stats) override { lastTotals = stats.totals; }
};

/// Applies the filter in @p environment's LOG to core::log, as endo's test mains do: `LOG=net`
/// enables the `net` category, disables every other one but `error`, and writes them to standard
/// output. An unset or empty LOG leaves the categories as they are.
void applyLogFilter(core::Environment const& environment)
{
    auto const filter = environment.get("LOG");
    if (!filter || filter->empty())
        return;
    core::log::configure(*filter);
    core::log::setFormatter(core::log::makeStandardFormatter({}));
    core::log::Sink::console().setEnabled(true);
}

} // namespace

CATCH_REGISTER_LISTENER(TotalsListener)

int main(int argc, char* argv[])
{
    core::testing::suppressWindowsDialogs();
    auto session = Catch::Session {};
    if (auto const rc = session.applyCommandLine(argc, argv); rc != 0)
        return rc;
    applyLogFilter(core::LiveEnvironment {});
    return core::testing::normalisedExitCode(lastTotals, session.run());
}
