// SPDX-License-Identifier: Apache-2.0
///
/// Which backend Linux builds: epoll by preference, poll(2) beside it. CMake compiles
/// exactly one DefaultBackend.cpp — this one here — which is how no `#ifdef` chooses a
/// backend (Ruling R40).
#include <core/net/IoBackend.hpp>
#include <core/net/linux/EpollBackend.hpp>
#include <core/net/posix/PollBackend.hpp>

#include <memory>

namespace core::net
{

BackendKind preferredBackendKind() noexcept
{
    return BackendKind::Epoll;
}

std::unique_ptr<IoBackend> makeBackend(BackendKind kind)
{
    switch (kind)
    {
        case BackendKind::Poll: return std::make_unique<PollBackend>();

        case BackendKind::Epoll: {
            auto backend = std::make_unique<EpollBackend>();
            // A backend whose kernel object failed to materialise would refuse every
            // attach; report it as unavailable so the caller can fall back.
            return backend->good() ? std::unique_ptr<IoBackend> { std::move(backend) } : nullptr;
        }

        // Not built here: kqueue is the BSDs', IOCP is Windows'.
        case BackendKind::Kqueue:
        case BackendKind::Iocp:

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
    if (auto native = makeBackend(preferredBackendKind()))
        return native;
    // poll(2) is always available and behaviourally identical, just costlier. The
    // fallback is silent by design: a caller has nothing to decide.
    return std::make_unique<PollBackend>();
}

} // namespace core::net
