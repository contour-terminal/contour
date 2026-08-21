// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/WorkingDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>

using namespace vtbackend;

namespace
{

constexpr auto ThisMachine =
    LocalIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };

/// Pushes one context carrying the given fields onto @p stack.
void push(ContextStack& stack,
          std::string_view id,
          ContextType type,
          std::string_view cwd = {},
          std::string_view machineId = {},
          std::string_view hostname = {})
{
    auto command = ContextCommand {};
    command.verb = ContextVerb::Start;
    command.identifier = id;
    if (type != ContextType::None)
    {
        command.type = type;
        command.present.enable(ContextField::Type);
    }
    if (!cwd.empty())
    {
        command.workingDirectory = cwd;
        command.present.enable(ContextField::WorkingDirectory);
    }
    if (!machineId.empty())
    {
        command.machineId = machineId;
        command.present.enable(ContextField::MachineId);
    }
    if (!hostname.empty())
    {
        command.hostname = hostname;
        command.present.enable(ContextField::Hostname);
    }
    stack.apply(command);
}

} // namespace

TEST_CASE("WorkingDirectory.nothing anywhere resolves to nothing", "[cwd]")
{
    auto const stack = ContextStack {};
    for (auto const purpose: { CwdPurpose::Spawn, CwdPurpose::Display, CwdPurpose::OpenLocally })
        CHECK(!resolveWorkingDirectory(stack, "", ThisMachine, purpose).has_value());
}

TEST_CASE("WorkingDirectory.a local context cwd wins over OSC 7 for every purpose", "[cwd]")
{
    auto stack = ContextStack {};
    push(stack, "shell", ContextType::Shell, "/home/user/project", ThisMachine.machineId);

    for (auto const purpose: { CwdPurpose::Spawn, CwdPurpose::Display, CwdPurpose::OpenLocally })
    {
        auto const resolved = resolveWorkingDirectory(stack, "file://zeta/home/user", ThisMachine, purpose);
        REQUIRE(resolved.has_value());
        CHECK(resolved->path == "/home/user/project");
        CHECK(resolved->source == CwdSource::ContextSignal);
    }
}

TEST_CASE("WorkingDirectory.OSC 7 answers when no context carries a cwd", "[cwd]")
{
    auto stack = ContextStack {};
    push(stack, "shell", ContextType::Shell); // no cwd= at all

    auto const resolved =
        resolveWorkingDirectory(stack, "file://zeta/home/user", ThisMachine, CwdPurpose::Spawn);
    REQUIRE(resolved.has_value());
    CHECK(resolved->path == "/home/user");
    CHECK(resolved->source == CwdSource::Osc7);
}

TEST_CASE("WorkingDirectory.an anonymous context cwd is refused for spawning", "[cwd]")
{
    // The ssh case: nothing emits a `remote` context today, so the REMOTE host's shim sends contexts
    // that look entirely local -- and its /home/bob may well exist here too. Unknown is not Local.
    auto stack = ContextStack {};
    push(stack, "shell", ContextType::Shell, "/home/bob"); // no machineid, no hostname

    CHECK(!resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Spawn).has_value());
    CHECK(!resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::OpenLocally).has_value());
}

TEST_CASE("WorkingDirectory.an anonymous context cwd is still shown to the user", "[cwd]")
{
    auto stack = ContextStack {};
    push(stack, "shell", ContextType::Shell, "/home/bob");

    auto const resolved = resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Display);
    REQUIRE(resolved.has_value());
    CHECK(resolved->path == "/home/bob");
    CHECK(resolved->locality == ContextLocality::Unknown);
}

TEST_CASE("WorkingDirectory.a container cwd is displayed but never spawned into", "[cwd]")
{
    // The concrete hazard: a container path carries NO host authority, so running it through
    // isLocalHost() -- which accepts an empty authority -- would call it local and open the HOST's
    // directory of the same name.
    auto stack = ContextStack {};
    push(stack, "box", ContextType::Container, {}, ThisMachine.machineId, ThisMachine.hostname);
    push(stack, "shell", ContextType::Shell, "/app");

    auto const shown = resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Display);
    REQUIRE(shown.has_value());
    CHECK(shown->path == "/app");
    CHECK(shown->locality == ContextLocality::Foreign);

    CHECK(!resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Spawn).has_value());
    CHECK(!resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::OpenLocally).has_value());
}

TEST_CASE("WorkingDirectory.a foreign machine id is refused for spawning", "[cwd]")
{
    auto stack = ContextStack {};
    push(stack, "shell", ContextType::Shell, "/home/bob", "ffffffffffffffffffffffffffffffff", "zeta");

    CHECK(!resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Spawn).has_value());

    auto const shown = resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Display);
    REQUIRE(shown.has_value());
    CHECK(shown->locality == ContextLocality::Foreign);
}

TEST_CASE("WorkingDirectory.a foreign context falls through to OSC 7 for spawning", "[cwd]")
{
    // The fall-through is not a second chance at the same question: OSC 7 is a different source, with
    // its own host authority to test.
    auto stack = ContextStack {};
    push(stack, "box", ContextType::Container, "/app");

    auto const resolved =
        resolveWorkingDirectory(stack, "file://zeta/home/user", ThisMachine, CwdPurpose::Spawn);
    REQUIRE(resolved.has_value());
    CHECK(resolved->path == "/home/user");
    CHECK(resolved->source == CwdSource::Osc7);
}

TEST_CASE("WorkingDirectory.a remote OSC 7 url is refused for spawning but shown", "[cwd]")
{
    auto const stack = ContextStack {};

    CHECK(!resolveWorkingDirectory(stack, "file://elsewhere/home/bob", ThisMachine, CwdPurpose::Spawn)
               .has_value());

    auto const shown =
        resolveWorkingDirectory(stack, "file://elsewhere/home/bob", ThisMachine, CwdPurpose::Display);
    REQUIRE(shown.has_value());
    CHECK(shown->path == "/home/bob");
    CHECK(shown->locality == ContextLocality::Foreign);
}

TEST_CASE("WorkingDirectory.the walk skips a context that never mentioned a cwd", "[cwd]")
{
    // systemd sends type=command without a cwd when nothing changed, so the command inherits the
    // shell's.
    auto stack = ContextStack {};
    push(stack, "shell", ContextType::Shell, "/home/user", ThisMachine.machineId);
    push(stack, "cmd", ContextType::Command);

    auto const resolved = resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Spawn);
    REQUIRE(resolved.has_value());
    CHECK(resolved->path == "/home/user");
}

TEST_CASE("WorkingDirectory.spawn and openLocally always agree", "[cwd]")
{
    // The menu row's enablement and the action behind it go through this one function, so they cannot
    // disagree about whether a directory is openable.
    auto const cases = std::array {
        std::pair { std::string_view { "/home/user" }, ThisMachine.machineId },
        std::pair { std::string_view { "/app" }, std::string_view {} },
        std::pair { std::string_view { "/home/bob" },
                    std::string_view { "ffffffffffffffffffffffffffffffff" } },
    };

    for (auto const& [cwd, machineId]: cases)
    {
        auto stack = ContextStack {};
        push(stack, "shell", ContextType::Shell, cwd, machineId);
        auto const spawn = resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::Spawn);
        auto const open = resolveWorkingDirectory(stack, "", ThisMachine, CwdPurpose::OpenLocally);
        CHECK(spawn.has_value() == open.has_value());
        if (spawn && open)
            CHECK(spawn->path == open->path);
    }
}
