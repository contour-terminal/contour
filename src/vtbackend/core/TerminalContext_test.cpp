// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// A `start=` naming @p id and carrying @p type, plus whatever else the caller sets afterwards.
ContextCommand startOf(std::string_view id, ContextType type = ContextType::None)
{
    auto command = ContextCommand {};
    command.verb = ContextVerb::Start;
    command.identifier = id;
    if (type != ContextType::None)
    {
        command.type = type;
        command.present.enable(ContextField::Type);
    }
    return command;
}

/// A fully-specified outcome, so no designated initializer leaves a field unnamed.
ContextOutcome makeOutcome(ContextExit exit, ContextSignal signal = ContextSignal::None, uint64_t status = 0)
{
    return ContextOutcome { .exit = exit, .signal = signal, .status = status };
}

/// An `end=` naming @p id with no outcome fields.
ContextCommand endOf(std::string_view id)
{
    auto command = ContextCommand {};
    command.verb = ContextVerb::End;
    command.identifier = id;
    return command;
}

/// Adds `cwd=` to @p command, which is what makes the nearest-cwd walk stop at it.
ContextCommand& withCwd(ContextCommand& command, std::string_view cwd)
{
    command.workingDirectory = cwd;
    command.present.enable(ContextField::WorkingDirectory);
    return command;
}

/// Adds `machineid=` to @p command.
ContextCommand& withMachineId(ContextCommand& command, std::string_view machineId)
{
    command.machineId = machineId;
    command.present.enable(ContextField::MachineId);
    return command;
}

/// Adds `hostname=` to @p command.
ContextCommand& withHostname(ContextCommand& command, std::string_view hostname)
{
    command.hostname = hostname;
    command.present.enable(ContextField::Hostname);
    return command;
}

constexpr auto ThisMachine =
    LocalIdentity { .machineId = "3deb5353d3ba43d08201c136a47ead7b", .hostname = "zeta" };

} // namespace

// {{{ vocabulary tables

TEST_CASE("ContextType.every type round-trips through its wire spelling", "[context]")
{
    for (auto const type: ContextTypeList)
    {
        auto const name = contextTypeName(type);
        CHECK(!name.empty());
        CHECK(contextTypeFrom(name) == type);
    }
    CHECK(contextTypeName(ContextType::None).empty());
    CHECK(contextTypeFrom("nonesuch") == ContextType::None);
    CHECK(contextTypeFrom("") == ContextType::None);
}

TEST_CASE("ContextType.only vm, container, remote and boot cross a host boundary", "[context]")
{
    CHECK(contextTypeCrossesHost(ContextType::Vm) == HostBoundary::Crossed);
    CHECK(contextTypeCrossesHost(ContextType::Container) == HostBoundary::Crossed);
    CHECK(contextTypeCrossesHost(ContextType::Remote) == HostBoundary::Crossed);
    CHECK(contextTypeCrossesHost(ContextType::Boot) == HostBoundary::Crossed);

    CHECK(contextTypeCrossesHost(ContextType::Shell) == HostBoundary::Same);
    CHECK(contextTypeCrossesHost(ContextType::Command) == HostBoundary::Same);
    CHECK(contextTypeCrossesHost(ContextType::Elevate) == HostBoundary::Same);
    CHECK(contextTypeCrossesHost(ContextType::None) == HostBoundary::Same);
}

TEST_CASE("ContextType.a boundary context is a privilege or machine change", "[context]")
{
    CHECK(isBoundaryContext(ContextType::Elevate));
    CHECK(isBoundaryContext(ContextType::ChangePrivileges));
    CHECK(isBoundaryContext(ContextType::Container));
    CHECK(isBoundaryContext(ContextType::Vm));
    CHECK(isBoundaryContext(ContextType::Remote));

    CHECK(!isBoundaryContext(ContextType::Shell));
    CHECK(!isBoundaryContext(ContextType::Command));
    CHECK(!isBoundaryContext(ContextType::Session));
    CHECK(!isBoundaryContext(ContextType::None));
}

TEST_CASE("ContextExit.every exit kind round-trips through its wire spelling", "[context]")
{
    for (auto const exit:
         { ContextExit::Success, ContextExit::Failure, ContextExit::Crash, ContextExit::Interrupt })
    {
        auto const name = contextExitName(exit);
        CHECK(!name.empty());
        CHECK(contextExitFrom(name) == exit);
    }
    CHECK(contextExitName(ContextExit::Unknown).empty());
    CHECK(contextExitFrom("nonesuch") == ContextExit::Unknown);
}

TEST_CASE("ContextSignal.symbolic names decode to the numbers Linux gives them", "[context]")
{
    // Hard-coded rather than taken from <signal.h>: a context may describe a process on another
    // machine, so SIGSEGV must decode to 11 even on a platform that numbers it differently.
    CHECK(contextSignalFrom("SIGSEGV") == ContextSignal::Segv);
    CHECK(static_cast<int>(ContextSignal::Segv) == 11);
    CHECK(contextSignalFrom("SIGINT") == ContextSignal::Int);
    CHECK(static_cast<int>(ContextSignal::Int) == 2);
    CHECK(contextSignalFrom("SIGKILL") == ContextSignal::Kill);
    CHECK(static_cast<int>(ContextSignal::Kill) == 9);

    CHECK(contextSignalFrom("SIGNOSUCH") == ContextSignal::None);
    CHECK(contextSignalFrom("") == ContextSignal::None);
    CHECK(contextSignalName(ContextSignal::None).empty());
    CHECK(contextSignalName(ContextSignal::Segv) == "SIGSEGV");
}

TEST_CASE("ContextSignal.a peer-supplied number is validated, not cast", "[context]")
{
    CHECK(isKnownContextSignal(0));
    CHECK(isKnownContextSignal(11));
    CHECK(isKnownContextSignal(31));
    CHECK(!isKnownContextSignal(200));
    CHECK(!isKnownContextSignal(16));
}

TEST_CASE("ContextField.the mask names exactly the assigned bits", "[context]")
{
    auto expected = uint16_t {};
    for (auto const field: ContextFieldList)
        expected = static_cast<uint16_t>(expected | static_cast<uint16_t>(field));
    CHECK(ContextFieldMask == expected);
    CHECK(ContextFieldList.size() == 15);
}

TEST_CASE("ContextOutcome.exit codes map the way a shell would report them", "[context]")
{
    CHECK(makeOutcome(ContextExit::Success).asShellExitCode() == 0);
    CHECK(makeOutcome(ContextExit::Failure, ContextSignal::None, 1).asShellExitCode() == 1);
    CHECK(makeOutcome(ContextExit::Failure, ContextSignal::None, 42).asShellExitCode() == 42);
    // 128+n, as every POSIX shell spells a signal death.
    CHECK(makeOutcome(ContextExit::Failure, ContextSignal::Segv).asShellExitCode() == 139);
    CHECK(makeOutcome(ContextExit::Interrupt).asShellExitCode() == 130);
    CHECK(ContextOutcome {}.asShellExitCode() == 0);
}

// }}}
// {{{ the transitions

TEST_CASE("ContextStack.a fresh stack has no active context", "[context]")
{
    auto stack = ContextStack {};
    CHECK(stack.depth() == 0);
    CHECK(!stack.activeId());
    CHECK(stack.active() == nullptr);
    CHECK(stack.retainedCount() == 0);
    CHECK(stack.find(ContextId { 1 }) == nullptr);
}

TEST_CASE("ContextStack.start of an unknown id pushes", "[context]")
{
    auto stack = ContextStack {};
    auto const transition = stack.apply(startOf("shell-a", ContextType::Shell));

    CHECK(transition.kind == ContextTransitionKind::Pushed);
    CHECK(transition.change == ContextChange::Yes);
    CHECK(transition.subjectType == ContextType::Shell);
    CHECK(stack.depth() == 1);
    CHECK(stack.activeId() == transition.subject);
    REQUIRE(stack.active() != nullptr);
    CHECK(stack.active()->identifier == "shell-a");
    CHECK(!stack.active()->parent); // the outermost context has no parent
}

TEST_CASE("ContextStack.a push records its parent", "[context]")
{
    auto stack = ContextStack {};
    auto const shell = stack.apply(startOf("shell-a", ContextType::Shell)).subject;
    auto const command = stack.apply(startOf("cmd-1", ContextType::Command)).subject;

    CHECK(stack.depth() == 2);
    REQUIRE(stack.find(command) != nullptr);
    CHECK(stack.find(command)->parent == shell);
}

TEST_CASE("ContextStack.start of the active id updates in place and keeps the id", "[context]")
{
    // Load-bearing: systemd re-announces the shell context on EVERY prompt. Minting a fresh id would
    // exhaust the 16-bit space in ~32k prompts and point every scrollback line at a distinct record.
    auto stack = ContextStack {};
    auto const first = stack.apply(startOf("shell-a", ContextType::Shell)).subject;

    auto update = startOf("shell-a", ContextType::Shell);
    withCwd(update, "/home/user");
    auto const transition = stack.apply(update);

    CHECK(transition.kind == ContextTransitionKind::Updated);
    CHECK(transition.subject == first);
    CHECK(stack.depth() == 1);
    CHECK(stack.retainedCount() == 1);
    CHECK(stack.active()->workingDirectory == "/home/user");
}

TEST_CASE("ContextStack.an update flushes omitted fields to defaults", "[context]")
{
    auto stack = ContextStack {};
    auto first = startOf("shell-a", ContextType::Shell);
    withCwd(first, "/home/user");
    withHostname(first, "zeta");
    stack.apply(first);
    REQUIRE(stack.active()->workingDirectory == "/home/user");

    // The spec: "any previously set metadata fields are flushed out, reset to their defaults, and then
    // reinitialized from the newly supplied data".
    stack.apply(startOf("shell-a", ContextType::Shell));

    CHECK(stack.active()->workingDirectory.empty());
    CHECK(stack.active()->hostname.empty());
    CHECK(!(stack.active()->present & ContextField::WorkingDirectory).any());
}

TEST_CASE("ContextStack.an update that changes nothing reports no change", "[context]")
{
    // The systemd hot path: a shell context re-announced every prompt with byte-identical metadata.
    // Reporting a change here would redraw the frontend once per prompt for nothing.
    auto stack = ContextStack {};
    auto command = startOf("shell-a", ContextType::Shell);
    withCwd(command, "/home/user");
    withMachineId(command, ThisMachine.machineId);
    stack.apply(command);

    auto const before = stack.revision();
    auto const transition = stack.apply(command);

    CHECK(transition.kind == ContextTransitionKind::Updated);
    CHECK(transition.change == ContextChange::None);
    CHECK(stack.revision() == before);
}

TEST_CASE("ContextStack.an update clears an outcome recorded from a previous life", "[context]")
{
    auto stack = ContextStack {};
    stack.apply(startOf("shell-a", ContextType::Shell));
    auto cmd = startOf("cmd-1", ContextType::Command);
    stack.apply(cmd);

    auto finish = endOf("cmd-1");
    finish.outcome = makeOutcome(ContextExit::Failure, ContextSignal::None, 1);
    stack.apply(finish);

    // The same identifier starts again -- as a fresh push, since it left the ancestry.
    auto const reborn = stack.apply(startOf("cmd-1", ContextType::Command)).subject;
    CHECK(stack.find(reborn)->outcome == ContextOutcome {});
}

TEST_CASE("ContextStack.start of an ancestor returns to it and terminates subcontexts", "[context]")
{
    auto stack = ContextStack {};
    auto const shell = stack.apply(startOf("shell-a", ContextType::Shell)).subject;
    stack.apply(startOf("cmd-1", ContextType::Command));
    stack.apply(startOf("cmd-2", ContextType::Command));
    REQUIRE(stack.depth() == 3);

    auto const transition = stack.apply(startOf("shell-a", ContextType::Shell));

    CHECK(transition.kind == ContextTransitionKind::ReturnedTo);
    CHECK(transition.subject == shell);
    CHECK(transition.implicitlyEnded == 2);
    CHECK(transition.change == ContextChange::Yes);
    CHECK(stack.depth() == 1);
    CHECK(stack.activeId() == shell);
}

TEST_CASE("ContextStack.start of an ended id pushes a new context rather than resurrecting", "[context]")
{
    // Minting a new id keeps every line's stamp meaning what it meant when it was written: a scrollback
    // line pointing at the old record must not silently acquire the new context's metadata.
    auto stack = ContextStack {};
    stack.apply(startOf("shell-a", ContextType::Shell));
    auto const firstCommand = stack.apply(startOf("cmd-1", ContextType::Command)).subject;
    stack.apply(endOf("cmd-1"));

    auto const transition = stack.apply(startOf("cmd-1", ContextType::Command));

    CHECK(transition.kind == ContextTransitionKind::Pushed);
    CHECK(transition.subject != firstCommand);
    CHECK(stack.find(firstCommand) != nullptr); // the old record is still resolvable
}

TEST_CASE("ContextStack.end of the active id pops", "[context]")
{
    auto stack = ContextStack {};
    auto const shell = stack.apply(startOf("shell-a", ContextType::Shell)).subject;
    auto const command = stack.apply(startOf("cmd-1", ContextType::Command)).subject;

    auto finish = endOf("cmd-1");
    finish.outcome = makeOutcome(ContextExit::Failure, ContextSignal::Segv, 139);
    auto const transition = stack.apply(finish);

    CHECK(transition.kind == ContextTransitionKind::Ended);
    CHECK(transition.subject == command);
    CHECK(transition.implicitlyEnded == 0);
    CHECK(stack.activeId() == shell);
    REQUIRE(stack.find(command) != nullptr);
    CHECK(stack.find(command)->outcome.signal == ContextSignal::Segv);
}

TEST_CASE("ContextStack.end of an ancestor pops it and everything below", "[context]")
{
    // The spec is silent here. Ignoring it would leave a TERMINATED context in the ancestry, which
    // makes the chain a lie; popping the ancestor and its descendants is the only reading that keeps a
    // stack a stack.
    auto stack = ContextStack {};
    stack.apply(startOf("outer", ContextType::Container));
    auto const shell = stack.apply(startOf("shell-a", ContextType::Shell)).subject;
    stack.apply(startOf("cmd-1", ContextType::Command));
    REQUIRE(stack.depth() == 3);

    auto const transition = stack.apply(endOf("shell-a"));

    CHECK(transition.kind == ContextTransitionKind::Ended);
    CHECK(transition.subject == shell);
    CHECK(transition.implicitlyEnded == 1);
    CHECK(stack.depth() == 1);
    CHECK(stack.active()->identifier == "outer");
}

TEST_CASE("ContextStack.end of an unknown id does nothing", "[context]")
{
    // The protocol's stated safety property in code: if an unknown end could pop, a program could
    // terminate the context established above it by guessing an identifier.
    auto stack = ContextStack {};
    auto const shell = stack.apply(startOf("shell-a", ContextType::Shell)).subject;
    auto const before = stack.revision();

    auto const transition = stack.apply(endOf("nonesuch"));

    CHECK(transition.kind == ContextTransitionKind::Ignored);
    CHECK(transition.change == ContextChange::None);
    CHECK(!transition.subject);
    CHECK(stack.activeId() == shell);
    CHECK(stack.revision() == before);
}

TEST_CASE("ContextStack.end of a retired id does nothing", "[context]")
{
    auto stack = ContextStack {};
    stack.apply(startOf("shell-a", ContextType::Shell));
    stack.apply(startOf("cmd-1", ContextType::Command));
    stack.apply(endOf("cmd-1"));
    REQUIRE(stack.depth() == 1);

    CHECK(stack.apply(endOf("cmd-1")).kind == ContextTransitionKind::Ignored);
    CHECK(stack.depth() == 1);
}

TEST_CASE("ContextStack.end of the last context leaves the ancestry empty", "[context]")
{
    // Empty is also the state at session start, so allowing it means one state fewer, not one more.
    // There is no terminal-owned bottom entry: unlike a pointer shape, the terminal knows nothing about
    // the application's process tree that it could truthfully name.
    auto stack = ContextStack {};
    stack.apply(startOf("shell-a", ContextType::Shell));

    CHECK(stack.apply(endOf("shell-a")).kind == ContextTransitionKind::Ended);
    CHECK(stack.depth() == 0);
    CHECK(!stack.activeId());
    CHECK(stack.active() == nullptr);
}

TEST_CASE("ContextStack.a push at the depth limit keeps the earlier and discards the newer", "[context]")
{
    // The spec's rule, which is the OPPOSITE of MaxSavedTitles': a program deep in the ancestry must
    // not be able to evict the elevate context above it.
    auto stack = ContextStack { { .maxDepth = 3, .maxRetained = 64 } };
    stack.apply(startOf("a", ContextType::Elevate));
    stack.apply(startOf("b", ContextType::Shell));
    stack.apply(startOf("c", ContextType::Command));
    REQUIRE(stack.depth() == 3);

    auto const transition = stack.apply(startOf("d", ContextType::Command));

    CHECK(transition.kind == ContextTransitionKind::DepthExceeded);
    CHECK(transition.change == ContextChange::None);
    CHECK(!transition.subject);
    CHECK(stack.depth() == 3);
    CHECK(stack.droppedPushes() == 1);
    CHECK(stack.chain().front().record->identifier == "a"); // the earliest survived
    CHECK(stack.activeId() == stack.chain().back().record->id);
}

TEST_CASE("ContextStack.a refused push does not let its end pop the parent", "[context]")
{
    // A push refused for depth is unknown to the ancestry, so its later `end=` falls through the
    // unknown-identifier arm. That is why that arm has to be evaluated first.
    auto stack = ContextStack { { .maxDepth = 2, .maxRetained = 64 } };
    stack.apply(startOf("a", ContextType::Shell));
    auto const b = stack.apply(startOf("b", ContextType::Command)).subject;
    stack.apply(startOf("c", ContextType::Command));
    REQUIRE(stack.depth() == 2);

    CHECK(stack.apply(endOf("c")).kind == ContextTransitionKind::Ignored);
    CHECK(stack.depth() == 2);
    CHECK(stack.activeId() == b);
}

TEST_CASE("ContextStack.an update still works at the depth limit", "[context]")
{
    auto stack = ContextStack { { .maxDepth = 2, .maxRetained = 64 } };
    stack.apply(startOf("a", ContextType::Shell));
    stack.apply(startOf("b", ContextType::Command));

    auto update = startOf("b", ContextType::Command);
    withCwd(update, "/tmp");
    auto const transition = stack.apply(update);

    CHECK(transition.kind == ContextTransitionKind::Updated);
    CHECK(stack.active()->workingDirectory == "/tmp");
    CHECK(stack.droppedPushes() == 0);
}

TEST_CASE("ContextStack.a start with a different type is still an update", "[context]")
{
    // The identifier is the identity; the type is metadata like any other field.
    auto stack = ContextStack {};
    auto const id = stack.apply(startOf("x", ContextType::Shell)).subject;

    auto const transition = stack.apply(startOf("x", ContextType::App));

    CHECK(transition.kind == ContextTransitionKind::Updated);
    CHECK(transition.subject == id);
    CHECK(stack.active()->type == ContextType::App);
}

// }}}
// {{{ store and lifetime

TEST_CASE("ContextStack.a retired record stays findable by id", "[context]")
{
    // A scrolled-back line keeps its id after the context closed, so the record has to outlive `end=`.
    // If it did not, the tint would vanish retroactively the moment a command exits.
    auto stack = ContextStack {};
    stack.apply(startOf("shell-a", ContextType::Shell));
    auto const command = stack.apply(startOf("cmd-1", ContextType::Command)).subject;
    stack.apply(endOf("cmd-1"));

    REQUIRE(stack.find(command) != nullptr);
    CHECK(stack.find(command)->identifier == "cmd-1");
}

TEST_CASE("ContextStack.the oldest record is evicted first", "[context]")
{
    // FIFO rather than LRU: scrollback eviction is itself FIFO, so the oldest context is the one whose
    // lines are most likely already gone -- and LRU would mean mutating the cache on the render path.
    auto stack = ContextStack { { .maxDepth = 2, .maxRetained = 4 } };
    stack.apply(startOf("shell", ContextType::Shell));

    auto ids = std::vector<ContextId> {};
    for (auto const name: { "c1"sv, "c2"sv, "c3"sv, "c4"sv, "c5"sv })
    {
        ids.push_back(stack.apply(startOf(name, ContextType::Command)).subject);
        stack.apply(endOf(name));
    }

    CHECK(stack.retainedCount() <= 4);
    CHECK(stack.find(ids.front()) == nullptr); // c1 aged out
    CHECK(stack.find(ids.back()) != nullptr);  // c5 is still here
}

TEST_CASE("ContextStack.a context that is still an ancestor is never evicted", "[context]")
{
    auto stack = ContextStack { { .maxDepth = 2, .maxRetained = 2 } };
    auto const shell = stack.apply(startOf("shell", ContextType::Shell)).subject;

    for (auto const name: { "c1"sv, "c2"sv, "c3"sv, "c4"sv })
    {
        stack.apply(startOf(name, ContextType::Command));
        stack.apply(endOf(name));
    }

    // The ancestry holds a strong reference, so the shell survives every eviction pass.
    REQUIRE(stack.find(shell) != nullptr);
    CHECK(stack.find(shell)->identifier == "shell");
}

TEST_CASE("ContextStack.an evicted id resolves to nothing rather than failing", "[context]")
{
    // The defined answer, not an error -- exactly the contract HyperlinkStorage::hyperlinkById has.
    auto stack = ContextStack {};
    CHECK(stack.find(ContextId { 9999 }) == nullptr);
    CHECK(stack.retain(ContextId { 9999 }) == nullptr);
    CHECK(stack.find(ContextId {}) == nullptr);
}

TEST_CASE("ContextStack.retain keeps a record alive past eviction", "[context]")
{
    auto stack = ContextStack { { .maxDepth = 2, .maxRetained = 2 } };
    stack.apply(startOf("shell", ContextType::Shell));
    auto const doomed = stack.apply(startOf("c1", ContextType::Command)).subject;
    stack.apply(endOf("c1"));

    auto const held = stack.retain(doomed);
    REQUIRE(held != nullptr);

    for (auto const name: { "c2"sv, "c3"sv, "c4"sv })
    {
        stack.apply(startOf(name, ContextType::Command));
        stack.apply(endOf(name));
    }

    CHECK(held->identifier == "c1"); // the caller's handle still resolves
}

TEST_CASE("ContextStack.clear drops the ancestry and every record", "[context]")
{
    // Reachable from session teardown only -- never from an escape sequence. RIS and DECSTR must not
    // clear the stack.
    auto stack = ContextStack {};
    stack.apply(startOf("shell", ContextType::Shell));
    auto const id = stack.apply(startOf("cmd", ContextType::Command)).subject;

    stack.clear();

    CHECK(stack.depth() == 0);
    CHECK(stack.retainedCount() == 0);
    CHECK(stack.find(id) == nullptr);
    CHECK(!stack.activeId());
}

TEST_CASE("ContextStack.the revision advances only on an observable change", "[context]")
{
    auto stack = ContextStack {};
    CHECK(stack.revision() == 0);

    stack.apply(startOf("a", ContextType::Shell));
    auto const afterPush = stack.revision();
    CHECK(afterPush > 0);

    stack.apply(startOf("a", ContextType::Shell)); // identical re-announce
    CHECK(stack.revision() == afterPush);

    stack.apply(endOf("nonesuch")); // ignored
    CHECK(stack.revision() == afterPush);

    stack.apply(endOf("a"));
    CHECK(stack.revision() > afterPush);
}

// }}}
// {{{ derived questions

TEST_CASE("ContextStack.the effective cwd is the nearest one walking up", "[context]")
{
    auto stack = ContextStack {};
    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/home/user");
    withMachineId(shell, ThisMachine.machineId);
    stack.apply(shell);

    auto command = startOf("cmd", ContextType::Command);
    withCwd(command, "/home/user/project");
    withMachineId(command, ThisMachine.machineId);
    stack.apply(command);

    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->path == "/home/user/project");
    CHECK(cwd->locality == ContextLocality::Local);
}

TEST_CASE("ContextStack.a context that sent no cwd is skipped by the walk", "[context]")
{
    // systemd sends `type=command` without a cwd when nothing changed, and the command must then
    // inherit the shell's.
    auto stack = ContextStack {};
    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/home/user");
    stack.apply(shell);
    stack.apply(startOf("cmd", ContextType::Command));

    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->path == "/home/user");
}

TEST_CASE("ContextStack.no cwd anywhere on the ancestry reports nothing", "[context]")
{
    auto stack = ContextStack {};
    stack.apply(startOf("shell", ContextType::Shell));
    stack.apply(startOf("cmd", ContextType::Command));

    CHECK(!stack.effectiveWorkingDirectory(ThisMachine).has_value());
}

TEST_CASE("ContextStack.an empty ancestry reports no cwd", "[context]")
{
    auto stack = ContextStack {};
    CHECK(!stack.effectiveWorkingDirectory(ThisMachine).has_value());
}

TEST_CASE("ContextStack.a container ancestor makes the active context foreign", "[context]")
{
    // A boundary crossed above is never escaped by a deeper context: a shell inside a container is
    // still inside it.
    auto stack = ContextStack {};
    auto container = startOf("box", ContextType::Container);
    withMachineId(container, ThisMachine.machineId); // even claiming OUR machine id
    stack.apply(container);

    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/app");
    withHostname(shell, ThisMachine.hostname); // and OUR hostname
    stack.apply(shell);

    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->path == "/app");
    CHECK(cwd->locality == ContextLocality::Foreign);
}

TEST_CASE("ContextStack.a differing machine id makes the context foreign", "[context]")
{
    // The strong test, and the one that catches ssh: NOTHING emits a `remote` context today, so a
    // remote shell's own sequences look entirely local and its /home/bob may well exist here too.
    auto stack = ContextStack {};
    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/home/bob");
    withMachineId(shell, "ffffffffffffffffffffffffffffffff");
    withHostname(shell, ThisMachine.hostname); // a similarly-named host does not save it
    stack.apply(shell);

    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->locality == ContextLocality::Foreign);
}

TEST_CASE("ContextStack.a differing hostname makes the context foreign when no machine id is given",
          "[context]")
{
    auto stack = ContextStack {};
    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/home/bob");
    withHostname(shell, "elsewhere");
    stack.apply(shell);

    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->locality == ContextLocality::Foreign);
}

TEST_CASE("ContextStack.an anonymous cwd is Unknown, not Local", "[context]")
{
    // The conservative answer, and it degrades to exactly the pre-OSC-3008 behaviour. Treating it as
    // Local would let an ssh session seed a local tab with a remote path that happens to exist.
    auto stack = ContextStack {};
    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/home/bob");
    stack.apply(shell);

    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->path == "/home/bob");
    CHECK(cwd->locality == ContextLocality::Unknown);
}

TEST_CASE("ContextStack.a plain shell on this machine is local", "[context]")
{
    auto stack = ContextStack {};
    auto shell = startOf("shell", ContextType::Shell);
    withCwd(shell, "/home/user");
    withMachineId(shell, ThisMachine.machineId);
    stack.apply(shell);

    CHECK(stack.localityOf(stack.activeId(), ThisMachine) == ContextLocality::Local);
}

TEST_CASE("ContextStack.localityOf an unknown id is Unknown", "[context]")
{
    auto stack = ContextStack {};
    CHECK(stack.localityOf(ContextId {}, ThisMachine) == ContextLocality::Unknown);
    CHECK(stack.localityOf(ContextId { 42 }, ThisMachine) == ContextLocality::Unknown);
}

TEST_CASE("ContextStack.localityOf a retired id is Unknown", "[context]")
{
    // It is off the ancestry, so nothing on the chain can speak to where it was.
    auto stack = ContextStack {};
    stack.apply(startOf("shell", ContextType::Shell));
    auto const cmd = stack.apply(startOf("cmd", ContextType::Command)).subject;
    stack.apply(endOf("cmd"));

    CHECK(stack.localityOf(cmd, ThisMachine) == ContextLocality::Unknown);
}

// }}}
// {{{ mirroring

TEST_CASE("ContextStack.adopt stores a record a mirror was handed", "[context]")
{
    auto stack = ContextStack {};
    auto record = TerminalContext {};
    record.id = ContextId { 7 };
    record.type = ContextType::Container;
    record.identifier = "box";
    record.container = "foobar";

    stack.adopt(record);

    REQUIRE(stack.find(ContextId { 7 }) != nullptr);
    CHECK(stack.find(ContextId { 7 })->container == "foobar");
}

TEST_CASE("ContextStack.adopting the same id twice updates in place", "[context]")
{
    // In place, so the ancestry's shared_ptr and every line already stamped with this id keep resolving
    // to the record they named.
    auto stack = ContextStack {};
    auto record = TerminalContext {};
    record.id = ContextId { 7 };
    record.type = ContextType::Shell;
    record.identifier = "s";
    stack.adopt(record);
    auto const* first = stack.find(ContextId { 7 });

    record.workingDirectory = "/tmp";
    stack.adopt(record);

    CHECK(stack.find(ContextId { 7 }) == first); // same object
    CHECK(stack.find(ContextId { 7 })->workingDirectory == "/tmp");
    CHECK(stack.retainedCount() == 1);
}

TEST_CASE("ContextStack.a locally created id never collides with an adopted one", "[context]")
{
    auto stack = ContextStack {};
    auto record = TerminalContext {};
    record.id = ContextId { 5 };
    record.type = ContextType::Shell;
    record.identifier = "adopted";
    stack.adopt(record);

    auto const minted = stack.apply(startOf("local", ContextType::Command)).subject;

    CHECK(minted != ContextId { 5 });
    CHECK(stack.find(ContextId { 5 })->identifier == "adopted");
}

TEST_CASE("ContextStack.setChain rebuilds the ancestry from ids", "[context]")
{
    auto stack = ContextStack {};
    auto outer = TerminalContext {};
    outer.id = ContextId { 1 };
    outer.type = ContextType::Container;
    outer.identifier = "box";
    auto inner = TerminalContext {};
    inner.id = ContextId { 2 };
    inner.parent = ContextId { 1 };
    inner.identifier = "sh";
    inner.type = ContextType::Shell;
    stack.adopt(outer);
    stack.adopt(inner);

    auto const ids = std::array { ContextId { 1 }, ContextId { 2 } };
    stack.setChain(ids);

    CHECK(stack.depth() == 2);
    CHECK(stack.activeId() == ContextId { 2 });
    CHECK(stack.chain().front().record->identifier == "box");
}

TEST_CASE("ContextStack.setChain skips ids it does not hold", "[context]")
{
    auto stack = ContextStack {};
    auto record = TerminalContext {};
    record.id = ContextId { 1 };
    record.type = ContextType::Shell;
    record.identifier = "sh";
    stack.adopt(record);

    auto const ids = std::array { ContextId { 1 }, ContextId { 99 } };
    stack.setChain(ids);

    CHECK(stack.depth() == 1);
}

TEST_CASE("ContextStack.setChain to what is already there reports no change", "[context]")
{
    auto stack = ContextStack {};
    auto record = TerminalContext {};
    record.id = ContextId { 1 };
    record.type = ContextType::Shell;
    record.identifier = "sh";
    stack.adopt(record);
    auto const ids = std::array { ContextId { 1 } };
    stack.setChain(ids);

    auto const before = stack.revision();
    stack.setChain(ids);

    CHECK(stack.revision() == before);
}

TEST_CASE("ContextStack.records reports every retained record oldest first", "[context]")
{
    auto stack = ContextStack {};
    stack.apply(startOf("a", ContextType::Shell));
    stack.apply(startOf("b", ContextType::Command));

    auto const records = stack.records();

    REQUIRE(records.size() == 2);
    CHECK(records[0].identifier == "a");
    CHECK(records[1].identifier == "b");
}

// }}}
// {{{ the real traffic

TEST_CASE("ContextStack.the systemd prompt cycle keeps one shell and one command per command", "[context]")
{
    // What /etc/profile.d/80-systemd-osc-context.sh actually emits, three commands deep. The shell
    // context is announced once and UPDATED every prompt; it is never ended.
    auto stack = ContextStack {};

    auto shellUpdate = [&](std::string_view cwd) {
        auto command = startOf("shell-uuid", ContextType::Shell);
        withCwd(command, cwd);
        withMachineId(command, ThisMachine.machineId);
        withHostname(command, ThisMachine.hostname);
        return command;
    };

    auto shellId = ContextId {};
    for (auto const& [index, name]:
         std::vector<std::pair<int, std::string_view>> { { 0, "cmd-1" }, { 1, "cmd-2" }, { 2, "cmd-3" } })
    {
        (void) index;
        // 1. PROMPT_COMMAND updates the shell context (a push the very first time).
        auto const shell = stack.apply(shellUpdate("/home/user"));
        if (!shellId)
            shellId = shell.subject;
        CHECK(shell.subject == shellId); // the same record every prompt, forever

        // 2. PS0 pushes the command context beneath it.
        auto commandStart = startOf(name, ContextType::Command);
        withCwd(commandStart, "/home/user");
        withMachineId(commandStart, ThisMachine.machineId);
        CHECK(stack.apply(commandStart).kind == ContextTransitionKind::Pushed);
        CHECK(stack.depth() == 2);

        // 3. the next PROMPT_COMMAND closes it.
        auto finish = endOf(name);
        finish.outcome = makeOutcome(ContextExit::Success);
        CHECK(stack.apply(finish).kind == ContextTransitionKind::Ended);
        CHECK(stack.depth() == 1);
    }

    CHECK(stack.activeId() == shellId);
    CHECK(stack.retainedCount() == 4); // one shell + three commands
}

TEST_CASE("ContextStack.a nested run0 inside a container reads as foreign and elevated", "[context]")
{
    auto stack = ContextStack {};
    auto container = startOf("box", ContextType::Container);
    container.container = "foobar";
    container.present.enable(ContextField::Container);
    stack.apply(container);

    auto elevate = startOf("root", ContextType::Elevate);
    elevate.targetUser = "root";
    elevate.present.enable(ContextField::TargetUser);
    stack.apply(elevate);

    auto shell = startOf("sh", ContextType::Shell);
    withCwd(shell, "/app");
    stack.apply(shell);

    CHECK(stack.depth() == 3);
    auto const cwd = stack.effectiveWorkingDirectory(ThisMachine);
    REQUIRE(cwd.has_value());
    CHECK(cwd->locality == ContextLocality::Foreign);
    CHECK(stack.chain().front().record->type == ContextType::Container);
}

// }}}
