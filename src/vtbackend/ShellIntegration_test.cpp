// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/ShellIntegration.h>
#include <vtbackend/Terminal.h>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

class MockShellIntegration: public ShellIntegration
{
  public:
    int promptStartCount = 0;
    bool lastPromptStartClickEvents = false;
    int promptEndCount = 0;
    int commandOutputStartCount = 0;
    std::optional<std::string> lastCommandOutputStartUrl;
    int commandFinishedCount = 0;
    int lastCommandFinishedExitCode = -1;

    void promptStart(bool clickEvents) override
    {
        promptStartCount++;
        lastPromptStartClickEvents = clickEvents;
    }

    void promptEnd() override { promptEndCount++; }

    void commandOutputStart(std::optional<std::string> const& commandLineUrl) override
    {
        commandOutputStartCount++;
        lastCommandOutputStartUrl = commandLineUrl;
    }

    void commandFinished(int exitCode) override
    {
        commandFinishedCount++;
        lastCommandFinishedExitCode = exitCode;
    }
};

} // namespace

TEST_CASE("ShellIntegration.OSC_133")
{
    auto mockUnique = std::make_unique<MockShellIntegration>();
    auto* mock = mockUnique.get();

    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };
    mc.terminal.setShellIntegration(std::move(mockUnique));

    SECTION("A: Prompt Start")
    {
        mc.writeToScreen("\033]133;A\033\\");
        CHECK(mock->promptStartCount == 1);
        CHECK(mock->lastPromptStartClickEvents == false);
    }

    SECTION("A: Prompt Start with click_events")
    {
        mc.writeToScreen("\033]133;A;click_events=1\033\\");
        CHECK(mock->promptStartCount == 1);
        CHECK(mock->lastPromptStartClickEvents == true);
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }

    SECTION("B: Prompt End")
    {
        mc.writeToScreen("\033]133;B\033\\");
        CHECK(mock->promptEndCount == 1);
    }

    SECTION("C: Command Output Start")
    {
        mc.writeToScreen("\033]133;C\033\\");
        CHECK(mock->commandOutputStartCount == 1);
        CHECK(mock->lastCommandOutputStartUrl == std::nullopt);
    }

    SECTION("C: Command Output Start with cmdline_url")
    {
        // "foo%20bar" unescapes to "foo bar"
        mc.writeToScreen("\033]133;C;cmdline_url=foo%20bar\033\\");
        CHECK(mock->commandOutputStartCount == 1);
        REQUIRE(mock->lastCommandOutputStartUrl.has_value());
        CHECK(mock->lastCommandOutputStartUrl.value() == "foo bar");
    }

    SECTION("D: Command Finished")
    {
        mc.writeToScreen("\033]133;D\033\\");
        CHECK(mock->commandFinishedCount == 1);
        CHECK(mock->lastCommandFinishedExitCode == 0);
    }

    SECTION("D: Command Finished with exit code")
    {
        mc.writeToScreen("\033]133;D;123\033\\");
        CHECK(mock->commandFinishedCount == 1);
        CHECK(mock->lastCommandFinishedExitCode == 123);
    }
}

TEST_CASE("ShellIntegration.SETMARK")
{
    auto mc = MockTerm { PageSize { LineCount(25), ColumnCount(80) } };

    SECTION("SETMARK triggers promptStart")
    {
        mc.writeToScreen("\033[>M");
        CHECK(mc.terminal.currentScreen().lineFlagsAt(LineOffset(0)).contains(LineFlag::Marked));
    }
}
