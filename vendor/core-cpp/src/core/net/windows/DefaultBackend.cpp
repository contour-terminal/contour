// SPDX-License-Identifier: Apache-2.0
///
/// Which backend Windows builds: an I/O completion port, and nothing else. poll(2) is not built
/// here: Winsock's `WSAPoll` is not poll(2), and cannot wait on a console handle or an event, which
/// is most of what a loop on this platform watches. CMake compiles exactly one DefaultBackend.cpp
/// — this one — which is how no `#ifdef` chooses a backend (Ruling R40).
///
/// **The completion port is the only Windows backend since 0.5.0.** It became the default in Task
/// B7b, with `IocpSocket`, `IocpListener` and the `ConnectEx` dial as the sockets that issue
/// overlapped operations on it, and the `WSAEventSelect` + `WaitForMultipleObjects` backend it
/// replaced stayed reachable by name for a release as the fallback. That release has passed, and
/// the fallback went with it ([core-cpp#6](https://github.com/contour-terminal/core-cpp/issues/6)):
/// a port the kernel refuses to create is handle exhaustion, which the fallback's own events hit
/// just as soon, and `makeDefaultBackend` now says so by throwing rather than by switching backends.
#include <core/net/IoBackend.hpp>
#include <core/net/windows/IocpBackend.hpp>

#include <memory>

namespace core::net
{

BackendKind preferredBackendKind() noexcept
{
    return BackendKind::Iocp;
}

std::unique_ptr<IoBackend> makeBackend(BackendKind kind)
{
    switch (kind)
    {
        case BackendKind::Iocp: return std::make_unique<IocpBackend>();

        // Not built here: poll(2), epoll and kqueue are the POSIX platforms'.
        case BackendKind::Poll:
        case BackendKind::Epoll:
        case BackendKind::Kqueue: return nullptr;

        // Reachable everywhere, and not through here — see posix/DefaultBackend.cpp.
        case BackendKind::HostDriven:
        case BackendKind::Scripted:
        case BackendKind::Null: return nullptr;

        case BackendKind::Last: break;
    }
    return nullptr;
}

std::unique_ptr<IoBackend> makeDefaultBackend()
{
    // Through makeBackend rather than straight to the constructor, so that this file is the same
    // shape as the other four. `IocpBackend` throws only when the port itself cannot be created,
    // which is handle exhaustion; that propagates, as `makeDefaultBackend` documents.
    return makeBackend(preferredBackendKind());
}

} // namespace core::net
