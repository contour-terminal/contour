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

    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
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
    // A value the probe holds a handle under, of no section: its own control pipe. Let go, never closed: the
    // probe still hears the next PDU on that pipe.
    REQUIRE_FALSE(probe.childHandles->empty());
    auto const forged = static_cast<uint64_t>(std::bit_cast<uintptr_t>(probe.childHandles->front()));
#else
    // The descriptor the request names never came. This pins only the end-to-end outcome: no value a POSIX
    // helper is sent can name a descriptor of its own, so there is nothing it could close by mistake.
    auto const forged = uint64_t { 0 };
#endif
    REQUIRE(control.send(instantiateNaming(forged)).has_value());

    auto const report = readReport(probe.lanes.front());
    REQUIRE(report.has_value());
    CHECK(*report == std::vector<std::string> { "granted=refused", "end" });

    // Its control channel survived the refusal: had the forged value been closed, the probe would hear
    // nothing.
    REQUIRE(control.send(proto::Release { .target = proto::ReleaseTarget::Instance, .id = 1 }).has_value());
    auto const heard = readReport(probe.lanes.front());
    REQUIRE(heard.has_value());
    CHECK(*heard == std::vector<std::string> { "heard", "end" });
    endProbe(probe, control);
}

TEST_CASE("HandleGrant.noTargetGrantsNothing", "[vtextension]")
{
    auto const object = transport::createSharedObject(ObjectBytes, transport::SharedObjectUse::GuestMemory);
    REQUIRE(object.has_value());
    CHECK(transport::HandleTarget::none().isNone());
    CHECK(transport::grantHandle(transport::HandleTarget::none(), object->get())
          == std::unexpected(transport::GrantError::Io));

    // A target moved on grants as it did; the one it was moved from is none().
    auto source = transport::HandleTarget::currentProcess();
    auto const moved = std::move(source);
    CHECK_FALSE(moved.isNone());
    auto const grant = transport::grantHandle(moved, object->get());
    REQUIRE(grant.has_value());
#ifdef _WIN32
    // Granted to this process: the duplicate is the test's to close.
    ::CloseHandle(std::bit_cast<HANDLE>(static_cast<uintptr_t>(grant->wireValue)));
#endif
}

#ifdef _WIN32
namespace
{

/// @return The helper's end of a fresh control channel, as the helper reads it.
[[nodiscard]] transport::PduStream helperControl(transport::ChannelPair& pair)
{
    return transport::PduStream {
        std::move(pair.helper), proto::ChannelKind::Control, proto::Direction::ToHelper, proto::PduLimits {}
    };
}

/// @return Whether @p handle names a handle of this process.
[[nodiscard]] bool isOpen(HANDLE handle)
{
    auto flags = DWORD { 0 };
    return ::GetHandleInformation(handle, &flags) != FALSE;
}

} // namespace

TEST_CASE("HandleGrant.claimRefusesWhatNamesNoHandle", "[vtextension]")
{
    auto pair = transport::makeChannelPair();
    REQUIRE(pair.has_value());
    auto stream = helperControl(*pair);
    // Not a multiple of 4; this process's and this thread's pseudo-handles; the process token's, a multiple
    // of 4.
    for (auto const value: { uint64_t { 0 }, uint64_t { 0x1F5 }, UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 3 })
    {
        INFO(value);
        CHECK(transport::claimGrantedHandle(stream, value)
              == std::unexpected(transport::StreamError::Malformed));
    }
}

TEST_CASE("HandleGrant.unprovedClaimIsNeverClosed", "[vtextension]")
{
    auto pair = transport::makeChannelPair();
    REQUIRE(pair.has_value());
    auto stream = helperControl(*pair);
    auto* const own = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(own != nullptr);
    auto const value = static_cast<uint64_t>(std::bit_cast<uintptr_t>(own));

    // A claim let go unproved forgets the value: it may name a handle of this process's own.
    {
        auto claim = transport::claimGrantedHandle(stream, value);
        REQUIRE(claim.has_value());
        CHECK(claim->get() == own);
    }
    CHECK(isOpen(own));

    // Adopted, once proved, it is this process's to close.
    {
        auto claim = transport::claimGrantedHandle(stream, value);
        REQUIRE(claim.has_value());
        auto const adopted = std::move(*claim).adopt();
        CHECK(adopted.get() == own);
    }
    CHECK_FALSE(isOpen(own));
}
#endif
