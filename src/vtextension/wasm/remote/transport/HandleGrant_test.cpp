// SPDX-License-Identifier: Apache-2.0
//
// Handing a shared memory object to an extension host process that runs already: what the probe is granted
// with an Instantiate is the object Contour maps, and what it is not granted it refuses.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#ifdef _WIN32
    #include <bit>
#endif

using namespace vtextension::wasm::remote;
using process::testing::ChildPatience;
using process::testing::endAndReap;
using process::testing::LaunchedHelper;
using process::testing::readReport;

namespace
{

/// The size of the object granted: the least an instance's memory object has.
constexpr auto ObjectBytes = proto::memoryObjectBytes(0);

/// What the probe writes at the start of what it was granted.
constexpr auto GrantMark = std::string_view { "probe" };

/// @return The probe, started to take one grant.
[[nodiscard]] LaunchedHelper launchGrantProbe()
{
    auto probe =
        process::testing::launchHelper(process::makeSystemProcessLauncher(),
                                       process::testing::HelperLaunch { .executable = VTEXTENSION_PROBE_PATH,
                                                                        .leading = { "--receive-grant" } });
    REQUIRE(probe.has_value());
    return *std::move(probe);
}

/// @return An Instantiate naming @p handle as an object of ObjectBytes.
[[nodiscard]] proto::Pdu instantiateNaming(uint64_t handle)
{
    return proto::Instantiate {
        .moduleId = 1,
        .instanceId = 1,
        .interruptSlot = 0,
        .memory =
            proto::MemoryGrant { .handle = handle, .objectBytes = ObjectBytes, .initialBytes = ObjectBytes },
    };
}

/// Ends the probe the way Contour lets a helper end, and checks that it exited as having reported.
void endProbe(LaunchedHelper& probe, transport::PduStream& control)
{
    control.shutdown();
    auto const status = process::testing::awaitReap(*probe.launcher, *probe.process);
    CHECK(status == process::ExitStatus { .kind = process::ExitKind::Exited, .value = 0 });
    if (!status)
        CHECK(endAndReap(*probe.launcher, *probe.process));
}

} // namespace

TEST_CASE("HandleGrant.reachesAChild", "[vtextension]")
{
    auto probe = launchGrantProbe();
    auto control = transport::PduStream {
        std::move(probe.control), proto::ChannelKind::Control, proto::Direction::ToParent, proto::PduLimits {}
    };
    auto const object = transport::createSharedObject(ObjectBytes, transport::SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    auto mapping = transport::SharedMapping::map(object->get(), ObjectBytes, transport::MapAccess::ReadWrite);
    REQUIRE(mapping.has_value());
    REQUIRE(mapping->commit(0, ObjectBytes).has_value());

    auto const target = probe.process->handleTarget();
    REQUIRE(target.has_value());
    auto const grant = transport::grantHandle(*target, object->get());
    REQUIRE(grant.has_value());
    REQUIRE(control.send(instantiateNaming(grant->wireValue), *grant).has_value());

    auto const report = readReport(probe.lanes.front());
    REQUIRE(report.has_value());
    CHECK(*report == std::vector<std::string> { "granted=ok", "end" });
    // What the probe wrote through its mapping shows through Contour's.
    auto const marked = [&] {
        return std::ranges::equal(mapping->bytes().first(GrantMark.size()),
                                  std::as_bytes(std::span { GrantMark }));
    };
    auto const deadline = std::chrono::steady_clock::now() + ChildPatience;
    while (!marked() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    CHECK(marked());
    endProbe(probe, control);
}

TEST_CASE("HandleGrant.forgedValueIsRefused", "[vtextension]")
{
    auto probe = launchGrantProbe();
    auto control = transport::PduStream {
        std::move(probe.control), proto::ChannelKind::Control, proto::Direction::ToParent, proto::PduLimits {}
    };
#ifdef _WIN32
    // A value the probe holds a handle under, of no section: its own control pipe. Released, not closed: the
    // probe still hears its control channel end below.
    REQUIRE_FALSE(probe.childHandles->empty());
    auto const forged = static_cast<uint64_t>(std::bit_cast<uintptr_t>(probe.childHandles->front()));
#else
    // The descriptor the request names never came.
    auto const forged = uint64_t { 0 };
#endif
    REQUIRE(control.send(instantiateNaming(forged)).has_value());

    auto const report = readReport(probe.lanes.front());
    REQUIRE(report.has_value());
    CHECK(*report == std::vector<std::string> { "granted=refused", "end" });
    endProbe(probe, control);
}
