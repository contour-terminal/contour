// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

#include <vthost/HostIdentity.hpp>

using vtbackend::HostIdentity;

namespace
{
/// The machine id the cases below run on.
constexpr auto MachineId = std::string_view { "3deb5353d3ba43d08201c136a47ead7b" };

/// @return The identity a daemon on the machine @p machine names states for it.
HostIdentity stated(HostIdentity const& machine)
{
    return vthost::appSpecificIdentity(machine.view());
}
} // namespace

TEST_CASE("Contour's application id is itself a 128-bit id", "[vthost][host]")
{
    CHECK(vtbackend::isMachineId(vthost::ContourApplicationId));
}

TEST_CASE("appSpecificMachineId derives the id systemd derives for Contour", "[vthost][host]")
{
    // The value `systemd-id128 machine-id --app-specific=<ContourApplicationId>` prints on a machine whose
    // /etc/machine-id is MachineId: HMAC-SHA256 keyed by the id over the application id, as a v4 UUID.
    CHECK(vthost::appSpecificMachineId(MachineId) == "babf933de97a49f0afa15e3e826cd9b5");
    // The id as a number, whichever case spells its digits.
    CHECK(vthost::appSpecificMachineId("3DEB5353D3BA43D08201C136A47EAD7B")
          == "babf933de97a49f0afa15e3e826cd9b5");
    // Another machine, another id.
    CHECK(vthost::appSpecificMachineId("77aa5353d3ba43d08201c136a47ead7b")
          == "e3f007755d4d4ae69ceb50f2e03f4cfe");
}

TEST_CASE("appSpecificMachineId keeps an unknown machine id unknown", "[vthost][host]")
{
    CHECK(vthost::appSpecificMachineId("").empty());
    CHECK(vthost::appSpecificMachineId("uninitialized").empty());
    CHECK(vthost::appSpecificMachineId("3deb5353-d3ba-43d0-8201-c136a47ead7b").empty());
}

TEST_CASE("appSpecificIdentity derives the machine id and keeps the host name", "[vthost][host]")
{
    auto const identity = vthost::appSpecificIdentity({ .machineId = MachineId, .hostname = "zeta" });

    CHECK(identity.machineId == "babf933de97a49f0afa15e3e826cd9b5");
    CHECK(identity.machineId != MachineId);
    CHECK(identity.hostname == "zeta");
}

TEST_CASE("the machine ids decide whether a daemon is this machine, when both are known", "[vthost][host]")
{
    auto const daemon = stated({ .machineId = std::string { MachineId }, .hostname = "zeta" });

    // Equal ids are one machine, whatever each calls itself: the client derives its own id as the daemon
    // derived the one it stated.
    CHECK(vthost::daemonMachineOf(daemon, { .machineId = MachineId, .hostname = "eta" })
          == vthost::DaemonMachine::Same);
    // Different ids are two, even under one name: a container or a cloned VM.
    CHECK(vthost::daemonMachineOf(daemon,
                                  { .machineId = "77aa5353d3ba43d08201c136a47ead7b", .hostname = "zeta" })
          == vthost::DaemonMachine::Other);
    // An id compares only with one derived the same way: the raw id itself proves nothing.
    CHECK(vthost::daemonMachineOf({ .machineId = std::string { MachineId }, .hostname = "zeta" },
                                  { .machineId = MachineId, .hostname = "zeta" })
          == vthost::DaemonMachine::Other);
}

TEST_CASE("a daemon with a machine id and a client without one are two machines", "[vthost][host]")
{
    // A daemon in WSL and the Windows GUI attached to it share a host name, but not a filesystem: the
    // daemon's /home/u/proj must not be opened on Windows.
    auto const wsl = HostIdentity { .machineId = std::string { MachineId }, .hostname = "DESKTOP-ABC" };
    auto const windows = HostIdentity { .machineId = {}, .hostname = "DESKTOP-ABC" };

    CHECK(vthost::daemonMachineOf(stated(wsl), windows.view()) == vthost::DaemonMachine::Other);
    CHECK(vthost::daemonMachineOf(stated(windows), wsl.view()) == vthost::DaemonMachine::Other);
}

TEST_CASE("the host names decide whether a daemon is this machine, when neither id is known",
          "[vthost][host]")
{
    auto const daemon = stated({ .machineId = {}, .hostname = "Zeta" });

    CHECK(vthost::daemonMachineOf(daemon, { .machineId = {}, .hostname = "zeta" })
          == vthost::DaemonMachine::Same);
    CHECK(vthost::daemonMachineOf(daemon, { .machineId = {}, .hostname = "eta" })
          == vthost::DaemonMachine::Other);
    // `uninitialized` is no machine id, so the names decide, as for a machine without one.
    CHECK(vthost::daemonMachineOf(stated({ .machineId = "uninitialized", .hostname = "zeta" }),
                                  { .machineId = "uninitialized", .hostname = "eta" })
          == vthost::DaemonMachine::Other);
}

TEST_CASE("an identity that proves nothing is another machine", "[vthost][host]")
{
    auto const self = vtbackend::LocalIdentity { .machineId = MachineId, .hostname = "zeta" };

    // A daemon that stated no identity: an empty name would pass isLocalHost(), which reads an empty URL
    // authority as this machine.
    CHECK(vthost::daemonMachineOf(HostIdentity {}, self) == vthost::DaemonMachine::Other);
    // And a client that knows nothing of itself.
    CHECK(
        vthost::daemonMachineOf(stated({ .machineId = {}, .hostname = "zeta" }), vtbackend::LocalIdentity {})
        == vthost::DaemonMachine::Other);
}

TEST_CASE("a daemon that calls itself localhost is no machine in particular", "[vthost][host]")
{
    // Every machine answers to the loopback name, so a daemon reporting it has said nothing about where it
    // runs. Over TCP from another machine, taking it for this one would hold the daemon's paths local here.
    auto const self = vtbackend::LocalIdentity { .machineId = {}, .hostname = "zeta" };
    CHECK(vthost::daemonMachineOf(stated({ .machineId = {}, .hostname = "localhost" }), self)
          == vthost::DaemonMachine::Other);
    CHECK(vthost::daemonMachineOf(stated({ .machineId = {}, .hostname = "LOCALHOST.localdomain" }), self)
          == vthost::DaemonMachine::Other);
    // Not even to a client that calls itself that too.
    CHECK(vthost::daemonMachineOf(stated({ .machineId = {}, .hostname = "localhost" }),
                                  { .machineId = {}, .hostname = "localhost" })
          == vthost::DaemonMachine::Other);
    // The machine ids still decide where both are known.
    CHECK(vthost::daemonMachineOf(stated({ .machineId = std::string { MachineId }, .hostname = "localhost" }),
                                  { .machineId = MachineId, .hostname = "zeta" })
          == vthost::DaemonMachine::Same);
}

TEST_CASE("a daemon and a context naming one machine agree on whether it is this one", "[vthost][host]")
{
    // A record's locality on a client follows the daemon's machine; the context the command ran in follows
    // the machine the context names. Where the two name the same host, a block and its context must not
    // disagree about whether the directory is here. (Where the client has a machine id and they name none,
    // they may: a daemon names every id it has, while a context may leave its id out and is then judged by
    // its host name -- vtbackend::MissingMachineId.)
    auto const contextLocality = [](HostIdentity const& named, vtbackend::LocalIdentity self) {
        auto stack = vtbackend::ContextStack {};
        auto shell = vtbackend::ContextCommand {};
        shell.verb = vtbackend::ContextVerb::Start;
        shell.identifier = "shell";
        shell.workingDirectory = "/home/user";
        shell.present.enable(vtbackend::ContextField::WorkingDirectory);
        if (!named.machineId.empty())
        {
            shell.machineId = named.machineId;
            shell.present.enable(vtbackend::ContextField::MachineId);
        }
        shell.hostname = named.hostname;
        shell.present.enable(vtbackend::ContextField::Hostname);
        stack.apply(shell);
        return stack.localityOf(stack.activeId(), self);
    };
    auto const pairs = {
        std::pair { HostIdentity { .machineId = {}, .hostname = "host" },
                    HostIdentity { .machineId = {}, .hostname = "host.lan" } },
        std::pair { HostIdentity { .machineId = {}, .hostname = "HOST" },
                    HostIdentity { .machineId = {}, .hostname = "host" } },
        std::pair { HostIdentity { .machineId = {}, .hostname = "localhost" },
                    HostIdentity { .machineId = {}, .hostname = "localhost" } },
        std::pair { HostIdentity { .machineId = std::string { MachineId }, .hostname = "host" },
                    HostIdentity { .machineId = {}, .hostname = "host" } },
        std::pair { HostIdentity { .machineId = std::string { MachineId }, .hostname = "host" },
                    HostIdentity { .machineId = std::string { MachineId }, .hostname = "host.lan" } },
    };
    for (auto const& [daemon, client]: pairs)
    {
        INFO("daemon " << daemon.machineId << "/" << daemon.hostname << ", client " << client.machineId << "/"
                       << client.hostname);
        auto const recordIsLocal =
            vthost::daemonMachineOf(stated(daemon), client.view()) == vthost::DaemonMachine::Same;
        auto const contextIsLocal =
            contextLocality(daemon, client.view()) == vtbackend::ContextLocality::Local;
        CHECK(recordIsLocal == contextIsLocal);
    }
}
