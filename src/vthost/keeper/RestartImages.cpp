// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/RestartImages.hpp>

#include <utility>

namespace vthost::keeper
{

namespace
{
    /// An image held by its lease; none for the installed executable of an install that runs in place.
    class LeasedImage final: public HeldImage
    {
      public:
        explicit LeasedImage(std::unique_ptr<ImageLease> lease) noexcept: _lease { std::move(lease) } {}

      private:
        std::unique_ptr<ImageLease> _lease;
    };
} // namespace

StoreRestartImages::StoreRestartImages(DaemonImageStore& store, std::filesystem::path installedExecutable):
    _store { store }, _installedExecutable { std::move(installedExecutable) }
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

std::expected<std::unique_ptr<HeldImage>, RestartImageRefusal> StoreRestartImages::hold(
    std::filesystem::path const& executable)
{
    if (_store.layout() == StagingLayout::InPlace)
    {
        // The one executable such an install runs, replaced where it lies; any other path is refused.
        if (_installedExecutable.empty()
            || executable.lexically_normal() != _installedExecutable.lexically_normal())
            return std::unexpected { RestartImageRefusal::NotAnImage };
        return std::make_unique<LeasedImage>(nullptr);
    }
    return _store.leaseImageOf(executable)
        .transform([](StagedImage image) -> std::unique_ptr<HeldImage> {
            return std::make_unique<LeasedImage>(std::move(image.lease));
        })
        .transform_error([](ImageLeaseError error) {
            switch (error)
            {
                case ImageLeaseError::NotAnImage: return RestartImageRefusal::NotAnImage;
                case ImageLeaseError::Busy: break;
            }
            return RestartImageRefusal::Unavailable;
        });
}

} // namespace vthost::keeper
