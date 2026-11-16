// SPDX-License-Identifier: Apache-2.0
#include <core/tui/CommandRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;

// ============================================================================
// Basic state tests
// ============================================================================

TEST_CASE("CommandRegistry.initial_state")
{
    CommandRegistry const reg;

    CHECK(reg.empty());
    CHECK(reg.empty());
    CHECK(reg.commands().empty());
}

TEST_CASE("CommandRegistry.add_and_find")
{
    CommandRegistry reg;
    auto invoked = false;

    reg.add({
        .id = "test.cmd",
        .label = "Test Command",
        .description = "A test command",
        .category = "Testing",
        .keybinding = "Ctrl+T",
        .context = CommandContext::Shell,
        .action = [&] { invoked = true; },
    });

    CHECK(reg.size() == 1);
    CHECK_FALSE(reg.empty());

    auto const* found = reg.find("test.cmd");
    REQUIRE(found != nullptr);
    CHECK(found->id == "test.cmd");
    CHECK(found->label == "Test Command");
    CHECK(found->description == "A test command");
    CHECK(found->category == "Testing");
    CHECK(found->keybinding == "Ctrl+T");
    CHECK(found->context == CommandContext::Shell);

    found->action();
    CHECK(invoked);
}

TEST_CASE("CommandRegistry.find_nonexistent")
{
    CommandRegistry const reg;

    CHECK(reg.find("does.not.exist") == nullptr);
}

TEST_CASE("CommandRegistry.add_replaces_duplicate_id")
{
    CommandRegistry reg;

    reg.add({
        .id = "test.cmd",
        .label = "Original",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Shell,
        .action = [] {},
    });
    reg.add({
        .id = "test.cmd",
        .label = "Replacement",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Agent,
        .action = [] {},
    });

    CHECK(reg.size() == 1);
    auto const* found = reg.find("test.cmd");
    REQUIRE(found != nullptr);
    CHECK(found->label == "Replacement");
    CHECK(found->context == CommandContext::Agent);
}

// ============================================================================
// Remove tests
// ============================================================================

TEST_CASE("CommandRegistry.remove")
{
    CommandRegistry reg;

    reg.add({
        .id = "a",
        .label = "A",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Shell,
        .action = [] {},
    });
    reg.add({
        .id = "b",
        .label = "B",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Agent,
        .action = [] {},
    });

    CHECK(reg.size() == 2);
    reg.remove("a");
    CHECK(reg.size() == 1);
    CHECK(reg.find("a") == nullptr);
    CHECK(reg.find("b") != nullptr);
}

TEST_CASE("CommandRegistry.remove_nonexistent_is_noop")
{
    CommandRegistry reg;

    reg.add({
        .id = "a",
        .label = "A",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Shell,
        .action = [] {},
    });

    reg.remove("nonexistent");
    CHECK(reg.size() == 1);
}

// ============================================================================
// Context filtering tests
// ============================================================================

TEST_CASE("CommandRegistry.commandsForContext_shell")
{
    CommandRegistry reg;

    reg.add({ .id = "shell_only",
              .label = "S",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Shell,
              .action = [] {} });
    reg.add({ .id = "agent_only",
              .label = "A",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Agent,
              .action = [] {} });
    reg.add({ .id = "both",
              .label = "B",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Both,
              .action = [] {} });

    auto const shellCmds = reg.commandsForContext(CommandContext::Shell);
    CHECK(shellCmds.size() == 2); // shell_only + both
    // Verify the expected commands are present
    auto hasShellOnly = false;
    auto hasBoth = false;
    for (auto const* cmd: shellCmds)
    {
        if (cmd->id == "shell_only")
            hasShellOnly = true;
        if (cmd->id == "both")
            hasBoth = true;
    }
    CHECK(hasShellOnly);
    CHECK(hasBoth);
}

TEST_CASE("CommandRegistry.commandsForContext_agent")
{
    CommandRegistry reg;

    reg.add({ .id = "shell_only",
              .label = "S",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Shell,
              .action = [] {} });
    reg.add({ .id = "agent_only",
              .label = "A",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Agent,
              .action = [] {} });
    reg.add({ .id = "both",
              .label = "B",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Both,
              .action = [] {} });

    auto const agentCmds = reg.commandsForContext(CommandContext::Agent);
    CHECK(agentCmds.size() == 2); // agent_only + both
    auto hasAgentOnly = false;
    auto hasBoth = false;
    for (auto const* cmd: agentCmds)
    {
        if (cmd->id == "agent_only")
            hasAgentOnly = true;
        if (cmd->id == "both")
            hasBoth = true;
    }
    CHECK(hasAgentOnly);
    CHECK(hasBoth);
}

TEST_CASE("CommandRegistry.commands_returns_all")
{
    CommandRegistry reg;

    reg.add({ .id = "a",
              .label = "A",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Shell,
              .action = [] {} });
    reg.add({ .id = "b",
              .label = "B",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Agent,
              .action = [] {} });
    reg.add({ .id = "c",
              .label = "C",
              .description = "",
              .category = "",
              .keybinding = "",
              .context = CommandContext::Both,
              .action = [] {} });

    CHECK(reg.commands().size() == 3);
}
