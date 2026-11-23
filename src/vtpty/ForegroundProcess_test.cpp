// SPDX-License-Identifier: Apache-2.0
//
// Picking a console's foreground process from a process-table snapshot -- the job leader below the shells --
// the decision Process makes on Windows, tested on every platform; and the program-name comparison it uses.

#include <vtpty/ForegroundProcess.hpp>
#include <vtpty/ProgramName.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using vtpty::foregroundProcessOf;
using vtpty::ProcessEntry;

TEST_CASE("foregroundProcessOf.an_idle_shell_is_in_front", "[vtpty][foreground]")
{
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 4, .parentPid = 0, .creationTime = 1, .name = "System" },
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "pwsh.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "pwsh.exe");
}

TEST_CASE("foregroundProcessOf.the_command_a_shell_runs", "[vtpty][foreground]")
{
    // pwsh runs cmd, which runs ping; a background job older than cmd is not in front.
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 300, .parentPid = 200, .creationTime = 30, .name = "PING.EXE" },
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "pwsh.exe" },
        { .pid = 150, .parentPid = 100, .creationTime = 15, .name = "job.exe" },
        { .pid = 200, .parentPid = 100, .creationTime = 20, .name = "cmd.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "PING.EXE");
}

TEST_CASE("foregroundProcessOf.a_relay_is_named_not_what_it_runs", "[vtpty][foreground]")
{
    // ssh with a ProxyCommand: the proxy is ssh's child, and ssh -- a relay -- leads the job, as on POSIX.
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "pwsh.exe" },
        { .pid = 200, .parentPid = 100, .creationTime = 20, .name = "ssh.exe" },
        { .pid = 300, .parentPid = 200, .creationTime = 30, .name = "nc.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "ssh.exe");
}

TEST_CASE("foregroundProcessOf.through_nested_shells", "[vtpty][foreground]")
{
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "cmd.exe" },
        { .pid = 200, .parentPid = 100, .creationTime = 20, .name = "PowerShell.EXE" },
        { .pid = 300, .parentPid = 200, .creationTime = 30, .name = "ssh.exe" },
        { .pid = 400, .parentPid = 300, .creationTime = 40, .name = "ssh.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "ssh.exe");
    // Without the command, the inner shell is in front.
    CHECK(foregroundProcessOf(std::span { processes }.first(2), 100) == "PowerShell.EXE");
}

TEST_CASE("foregroundProcessOf.a_batch_wrapper_names_what_it_runs", "[vtpty][foreground]")
{
    // `npm` is npm.cmd, which cmd runs: cmd is a shell, so the walk names the node it starts. POSIX names
    // the leader `npm`; the pane header says `node.exe` here.
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "pwsh.exe" },
        { .pid = 200, .parentPid = 100, .creationTime = 20, .name = "cmd.exe" },
        { .pid = 300, .parentPid = 200, .creationTime = 30, .name = "node.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "node.exe");
}

TEST_CASE("foregroundProcessOf.a_session_that_runs_no_shell", "[vtpty][foreground]")
{
    // A profile that runs a program directly: it leads, whatever it starts.
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "claude.exe" },
        { .pid = 200, .parentPid = 100, .creationTime = 20, .name = "bash.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "claude.exe");
}

TEST_CASE("foregroundProcessOf.a_reused_parent_id_is_no_child", "[vtpty][foreground]")
{
    // Process 100's id was its parent's too, before that one ended: it is older than the shell, so no child.
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 7, .parentPid = 100, .creationTime = 5, .name = "orphan.exe" },
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "pwsh.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "pwsh.exe");
}

TEST_CASE("foregroundProcessOf.a_child_of_unknown_age_is_not_followed", "[vtpty][foreground]")
{
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 100, .parentPid = 50, .creationTime = 10, .name = "pwsh.exe" },
        { .pid = 200, .parentPid = 100, .creationTime = 0, .name = "protected.exe" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "pwsh.exe");
}

TEST_CASE("foregroundProcessOf.a_cycle_of_equal_times_ends", "[vtpty][foreground]")
{
    // Snapshots are not atomic: whatever they say, the walk ends.
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 100, .parentPid = 200, .creationTime = 10, .name = "sh" },
        { .pid = 200, .parentPid = 100, .creationTime = 10, .name = "bash" },
        { .pid = 300, .parentPid = 300, .creationTime = 20, .name = "zsh" },
    };
    CHECK(foregroundProcessOf(processes, 100) == "sh");
    CHECK(foregroundProcessOf(processes, 300) == "zsh");
}

TEST_CASE("foregroundProcessOf.an_ended_shell_names_nothing", "[vtpty][foreground]")
{
    auto const processes = std::vector<ProcessEntry> {
        { .pid = 4, .parentPid = 0, .creationTime = 1, .name = "System" },
    };
    CHECK_FALSE(foregroundProcessOf(processes, 100).has_value());
    CHECK_FALSE(foregroundProcessOf({}, 100).has_value());
}

TEST_CASE("ProgramName.isSameProgram", "[vtpty][foreground]")
{
    using vtpty::isSameProgram;
    using vtpty::programStem;
    CHECK(programStem("C:\\Windows\\System32\\OpenSSH\\ssh.exe") == "ssh");
    CHECK(programStem("/usr/bin/ssh") == "ssh");
    CHECK(programStem("PING.EXE") == "PING");
    CHECK(programStem(".exe").empty());
    CHECK(isSameProgram("pwsh", "pwsh.exe"));
    CHECK(isSameProgram("PING.EXE", "ping"));
    CHECK(isSameProgram("C:/Program Files/PuTTY/plink.exe", "plink"));
    CHECK(isSameProgram("zsh", "zsh"));
    CHECK_FALSE(isSameProgram("zsh", "bash"));
    CHECK_FALSE(isSameProgram("ssh", "ssh.exe.bak"));
    CHECK_FALSE(isSameProgram("", ".exe"));
    CHECK_FALSE(isSameProgram("", ""));
}
