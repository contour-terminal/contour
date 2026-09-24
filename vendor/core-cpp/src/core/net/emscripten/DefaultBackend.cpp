// SPDX-License-Identifier: Apache-2.0
///
/// Which backend single-threaded WebAssembly builds: the host-driven one, over the
/// browser's own timer, and nothing else. There is no thread to block and no
/// descriptor to poll here — readiness in a browser arrives through its own APIs and
/// reaches the loop as a `post`. CMake compiles exactly one DefaultBackend.cpp — this
/// one here — which is how no `#ifdef` chooses a backend (Ruling R40).
#include <core/net/HostDrivenBackend.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/emscripten/EmscriptenHostScheduler.hpp>

#include <memory>

namespace core::net
{

namespace
{
    /// The browser's timer, as one object for the process.
    ///
    /// A function-local static rather than an injected one because a no-argument
    /// factory has nowhere to take a host from, and because the thing it wraps IS
    /// process-wide: there is one browser event loop, `emscripten_async_call` reaches
    /// it from anywhere, and the scheduler holds no state of its own. A program that
    /// wants to inject its own host — a Qt application, a test — constructs
    /// @c HostDrivenBackend directly, which is why that constructor takes the seam.
    /// @return The process's browser scheduler.
    [[nodiscard]] EmscriptenHostScheduler& browserHost() noexcept
    {
        static auto host = EmscriptenHostScheduler {};
        return host;
    }
} // namespace

BackendKind preferredBackendKind() noexcept
{
    return BackendKind::HostDriven;
}

std::unique_ptr<IoBackend> makeBackend(BackendKind kind)
{
    switch (kind)
    {
        // The one kind this platform builds, and the only one that can come from a
        // factory here: the host is the browser, so there is nothing for a caller to
        // supply.
        case BackendKind::HostDriven: return std::make_unique<HostDrivenBackend>(browserHost());

        // Not built here. Every one of them needs a thread to block on, a descriptor
        // to poll, or a Windows handle.
        case BackendKind::Poll:
        case BackendKind::Epoll:
        case BackendKind::Kqueue:
        case BackendKind::Iocp:

        // Test doubles: a case constructs the one it wants — see posix/DefaultBackend.cpp.
        case BackendKind::Scripted:
        case BackendKind::Null: return nullptr;

        case BackendKind::Last: break;
    }
    return nullptr;
}

std::unique_ptr<IoBackend> makeDefaultBackend()
{
    return std::make_unique<HostDrivenBackend>(browserHost());
}

} // namespace core::net
