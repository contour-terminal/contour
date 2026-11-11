// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/RestartImages.hpp>

#include <utility>

namespace vthost::keeper
{

StoreRestartImages::StoreRestartImages(DaemonImageStore& store): _store { store }
{
}

std::filesystem::path StoreRestartImages::pick(std::filesystem::path const& committed)
{
    auto newer = _store.leaseNewerImage(committed);
    if (!newer)
    {
        // Nothing newer is complete, so nothing newer was leased either: the committed image is the one.
        _lease.reset();
        return committed;
    }
    // The new lease is held before the one it replaces is let go, so the image picked twice is never free.
    _lease = std::move(newer->lease);
    return std::move(newer->executable);
}

} // namespace vthost::keeper
