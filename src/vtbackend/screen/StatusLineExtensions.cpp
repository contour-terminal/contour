// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <memory>
#include <ranges>
#include <unordered_map>

namespace vtbackend
{

void StatusLinePublishSink::attach(IStatusLinePublishTarget& target) noexcept
{
    _target.store(&target, std::memory_order_seq_cst);
}

void StatusLinePublishSink::revoke() noexcept
{
    // Sequentially consistent with publish(): either a delivery saw the null target, or this sees it in
    // flight and waits for it. Nothing that started before the store can reach the target afterwards.
    _target.store(nullptr, std::memory_order_seq_cst);
    auto pending = _inFlight.load(std::memory_order_seq_cst);
    while (pending != 0)
    {
        _inFlight.wait(pending, std::memory_order_seq_cst);
        pending = _inFlight.load(std::memory_order_seq_cst);
    }
}

void StatusLinePublishSink::publish() noexcept
{
    _inFlight.fetch_add(1, std::memory_order_seq_cst);
    if (auto* const target = _target.load(std::memory_order_seq_cst))
        target->statusLineExtensionsPublished();
    // Only the last delivery out wakes a revoke(): it waits for none to be in flight.
    if (_inFlight.fetch_sub(1, std::memory_order_seq_cst) == 1)
        _inFlight.notify_all();
}

void rehomeImageFragments(Line& line, ImageRehome const& rehome)
{
    auto& fragments = line.storage().imageFragments;
    if (!fragments)
        return;

    auto rehomed = std::unordered_map<RasterizedImage const*, std::shared_ptr<RasterizedImage const>> {};
    for (auto& fragment: *fragments | std::views::values)
    {
        if (!fragment)
            continue;
        auto const& source = fragment->rasterizedImage();
        auto& target = rehomed[&source];
        if (!target)
            target = std::make_shared<RasterizedImage const>(rehome(source.image()),
                                                             source.alignmentPolicy(),
                                                             source.resizePolicy(),
                                                             source.defaultColor(),
                                                             source.cellSpan(),
                                                             source.cellSize(),
                                                             source.layer(),
                                                             source.imageOffset(),
                                                             source.imageSubSize());
        fragment = std::make_shared<ImageFragment>(target, fragment->offset());
    }
}

std::shared_ptr<Image const> detachedImage(Image const& image)
{
    return std::make_shared<Image const>(
        image.id(), image.format(), Image::Data(image.data()), image.size(), [](Image const*) {});
}

void clearHyperlinks(Line& line) noexcept
{
    for (auto& hyperlink: line.storage().hyperlinks)
        hyperlink = HyperlinkId {};
}

} // namespace vtbackend
