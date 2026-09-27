// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canaries that prove `testing::InMemorySocket` refuses a second read or write over a parked
/// one, exactly as every real socket does.
///
/// **A fake that let two writes interleave would be more permissive than the transport it stands
/// for**, which is the divergence `SocketClosedStates_test.cpp` exists to prevent -- and that test
/// cannot see this one, because an assertion aborts the process rather than failing a case. So each
/// mode is its own process, driven to the guarded call and judged on a MARKER printed to stderr
/// immediately before it, never on `WILL_FAIL`: `SocketContractCanary.cpp` says at length why a
/// canary that dies early must not read as one whose guard fired.
///
/// Runs in every build: the slot guards end the process in Release too (`core/net/SocketContract.hpp`).

#include <core/async/Task.hpp>
#include <core/net/testing/InMemorySocket.hpp>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

/// What the process exits with once a guard has fired; a SIGABRT becomes this, so ctest reads an
/// exit code rather than a signal (see `SocketContractCanary.cpp`).
constexpr int RefusedExitCode = 1;

extern "C" void onAbort(int signalNumber)
{
    std::ignore = signalNumber;
    std::_Exit(RefusedExitCode);
}

/// The PASS marker, naming the mode, flushed because the next statement aborts.
void announce(char const* mode)
{
    std::fputs("inmemory-socket-canary: ", stderr);
    std::fputs(mode, stderr);
    std::fputs(": reached the guarded call\n", stderr);
    std::fflush(stderr);
}

/// The FAIL marker: the guarded call returned.
void survived(char const* what)
{
    std::fputs("inmemory-socket-canary: SURVIVED -- ", stderr);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    std::fflush(stderr);
}

/// Every flow this canary drives, kept on the heap for the process's life.
///
/// **Held here rather than in locals, because two local tasks crashed under MSVC's Release build**:
/// the second flow's handle was a stack address, and its body faulted (0xC0000005) before it reached
/// the verb -- a canary dying for its own reason rather than the guard's, and the reading is that
/// the compiler elided the frames onto the caller's stack. A task moved into this container escapes
/// the caller, so its frame cannot be elided, and the mode then reaches its guard (cl-release,
/// clang-cl alike).
std::vector<core::async::Task<void>>& flows()
{
    static auto held = std::vector<core::async::Task<void>> {};
    return held;
}

/// Starts @p task, keeping it in @c flows().
/// @param task The flow to run until it first suspends.
/// @return Whether it ran to its end rather than parking.
bool startFinishes(core::async::Task<void> task)
{
    auto& held = flows().emplace_back(std::move(task));
    held.handle().resume();
    return held.done();
}

core::async::Task<void> readOnce(core::net::ISocket* socket, std::span<std::byte> buffer)
{
    std::ignore = co_await socket->read(buffer);
}

core::async::Task<void> writeOnce(core::net::ISocket* socket, std::span<std::byte const> bytes)
{
    std::ignore = co_await socket->write(bytes);
}

/// Parks a read, then arms a second one over it.
int provokeReadSlot(char const* mode)
{
    auto pair = core::net::testing::InMemorySocketPair::create();
    flows().reserve(2);
    auto first = std::array<std::byte, 4> {};
    if (startFinishes(readOnce(pair.server.get(), first)))
    {
        survived("the first read did not park, so nothing was provoked");
        return 2;
    }

    auto second = std::array<std::byte, 4> {};
    announce(mode);
    std::ignore = startFinishes(readOnce(pair.server.get(), second));
    survived("a second read was armed over a parked one");
    return 1;
}

/// Parks a write against a full bounded pipe, then arms a second one over it.
int provokeWriteSlot(char const* mode)
{
    auto pair = core::net::testing::InMemorySocketPair::create(2);
    auto const payload = std::array<std::byte, 8> {};
    flows().reserve(2);
    if (startFinishes(writeOnce(pair.client.get(), payload)))
    {
        survived("the first write did not park, so nothing was provoked");
        return 2;
    }

    announce(mode);
    std::ignore = startFinishes(writeOnce(pair.client.get(), payload));
    survived("a second write was armed over a parked one");
    return 1;
}

} // namespace

/// @param argc The argument count.
/// @param argv `fake-read-slot` or `fake-write-slot`.
/// @return Never, where the guard fires: it ends the process.
int main(int argc, char** argv)
{
    std::ignore = std::signal(SIGABRT, &onAbort);
    if (argc != 2)
    {
        std::fputs("usage: core-cpp-inmemory-socket-canary fake-read-slot|fake-write-slot\n", stderr);
        return 2;
    }
    if (std::strcmp(argv[1], "fake-read-slot") == 0)
        return provokeReadSlot(argv[1]);
    if (std::strcmp(argv[1], "fake-write-slot") == 0)
        return provokeWriteSlot(argv[1]);
    std::fputs("inmemory-socket-canary: unknown mode\n", stderr);
    return 2;
}
