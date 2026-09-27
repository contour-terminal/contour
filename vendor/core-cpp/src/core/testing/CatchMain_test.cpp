// SPDX-License-Identifier: Apache-2.0
//
// What core::testing_main's main() does before it runs the tests: apply the LOG filter to core::log.
// ctest runs this binary with LOG=net (src/core/testing/CMakeLists.txt); run by hand without it, or
// under node, which passes no environment into WebAssembly, the test case skips.

#include <core/Environment.hpp>
#include <core/log/LogStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <sstream>

namespace
{

// Constructed before main() runs, as a module's categories are, so the filter main() applies
// reaches them. `tui` starts enabled, so that it is disabled only if the filter is applied.
auto netLog = core::log::Category { "net", "A category LOG=net selects." };
auto tuiLog =
    core::log::Category { "tui", "A category LOG=net leaves out.", core::log::Category::State::Enabled };

/// Redirects std::cout into a buffer for as long as it lives.
class CapturedStandardOutput
{
  public:
    CapturedStandardOutput(): _previous { std::cout.rdbuf(_buffer.rdbuf()) } {}
    ~CapturedStandardOutput() { std::cout.rdbuf(_previous); }

    CapturedStandardOutput(CapturedStandardOutput const&) = delete;
    CapturedStandardOutput& operator=(CapturedStandardOutput const&) = delete;
    CapturedStandardOutput(CapturedStandardOutput&&) = delete;
    CapturedStandardOutput& operator=(CapturedStandardOutput&&) = delete;

    [[nodiscard]] std::string text() const { return _buffer.str(); }

  private:
    std::ostringstream _buffer;
    std::streambuf* _previous;
};

} // namespace

TEST_CASE("main() enables the log categories LOG names, and only those", "[testing]")
{
    if (core::LiveEnvironment {}.get("LOG") != "net")
        SKIP("needs LOG=net in the environment, as ctest runs this binary");

    CHECK(netLog.isEnabled());
    CHECK_FALSE(tuiLog.isEnabled());
}

TEST_CASE("main() writes the categories LOG enables to standard output", "[testing]")
{
    if (core::LiveEnvironment {}.get("LOG") != "net")
        SKIP("needs LOG=net in the environment, as ctest runs this binary");

    auto const output = CapturedStandardOutput {};
    netLog()("a line on net");
    tuiLog()("a line on tui");

    CHECK(output.text().contains("[net] a line on net"));
    CHECK_FALSE(output.text().contains("a line on tui"));
}
