// SPDX-License-Identifier: Apache-2.0
///
/// Which backend a POSIX platform with no scalable readiness primitive of its own
/// builds: poll(2), and nothing else. Linux, the BSDs, Windows and Emscripten each
/// have their own copy of this file beside their own backend, and CMake compiles
/// exactly one of them — which is how no `#ifdef` chooses a backend (Ruling R40).
#include <core/net/IoBackend.hpp>
#include <core/net/posix/PollBackend.hpp>

#include <memory>

namespace core::net
{

BackendKind preferredBackendKind() noexcept
{
    return BackendKind::Poll;
}

std::unique_ptr<IoBackend> makeBackend(BackendKind kind)
{
    switch (kind)
    {
        case BackendKind::Poll: return std::make_unique<PollBackend>();

        // Not built here. Epoll is Linux's, Kqueue the BSDs', and IOCP is
        // Windows'; a caller asking for one gets a null rather than a silent
        // substitution, so a test that means to exercise a specific backend skips
        // instead of testing the wrong one twice.
        case BackendKind::Epoll:
        case BackendKind::Kqueue:
        case BackendKind::Iocp:

        // Reachable everywhere, and not through here. HostDriven needs the
        // IHostScheduler its host provides, which a no-argument factory cannot supply
        // (except under Emscripten, where the host is the browser); Scripted and Null
        // are test doubles, and a production factory is the wrong way to reach one.
        case BackendKind::HostDriven:
        case BackendKind::Scripted:
        case BackendKind::Null: return nullptr;

        case BackendKind::Last: break;
    }
    return nullptr;
}

std::unique_ptr<IoBackend> makeDefaultBackend()
{
    // Through makeBackend rather than straight to the constructor, although poll(2) is
    // the only backend here and has no `good()` to fail: it keeps the five
    // DefaultBackend.cpp files one shape instead of two, so a platform that later gains
    // a second backend gains the `good()` check and the fallback with it rather than
    // having to remember them. Task B7 does exactly that to the Windows file.
    if (auto native = makeBackend(preferredBackendKind()))
        return native;
    return std::make_unique<PollBackend>();
}

} // namespace core::net
