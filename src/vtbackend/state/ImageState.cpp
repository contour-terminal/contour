// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/ImageState.hpp>

#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/state/StateValues.hpp>

#include <algorithm>
#include <ranges>
#include <set>

namespace vtbackend
{

namespace
{
    using detail::StateFailure;
} // namespace

// {{{ ImageTableBuilder
ImageTableBuilder::ImageTableBuilder(ImagePool const& pool)
{
    _table.nextImageId = pool.nextImageId();
    for (auto const& [name, image]: pool.linkedImages())
        _table.names.push_back({ .name = name, .image = addImage(image) });
}

ImageId ImageTableBuilder::addImage(std::shared_ptr<Image const> const& image)
{
    auto const [it, inserted] = _filedImages.try_emplace(image.get(), image->id());
    if (inserted)
        _table.images.push_back(
            { .id = image->id(), .format = image->format(), .data = image->data(), .size = image->size() });
    return it->second;
}

uint64_t ImageTableBuilder::rasterizedIndexOf(RasterizedImage const& raster)
{
    auto const [it, inserted] = _filedRasterized.try_emplace(&raster, _table.rasterized.size());
    if (inserted)
        _table.rasterized.push_back({ .image = addImage(raster.imagePointer()),
                                      .alignment = raster.alignmentPolicy(),
                                      .resize = raster.resizePolicy(),
                                      .defaultColor = raster.defaultColor(),
                                      .cellSpan = raster.cellSpan(),
                                      .cellSize = raster.cellSize(),
                                      .layer = raster.layer(),
                                      .offset = raster.imageOffset(),
                                      .subSize = raster.imageSubSize() });
    return it->second;
}

uint64_t ImageTableBuilder::fragmentId(std::shared_ptr<ImageFragment> const& fragment)
{
    auto const [it, inserted] = _filedFragments.try_emplace(fragment.get(), _table.fragments.size());
    if (inserted)
        _table.fragments.push_back(
            { .rasterized = rasterizedIndexOf(fragment->rasterizedImage()), .offset = fragment->offset() });
    return it->second;
}

ImageTableState ImageTableBuilder::finish() &&
{
    std::ranges::sort(_table.images, {}, &ImageState::id);
    return std::move(_table);
}
// }}}

// {{{ RestoredImageTable
std::expected<RestoredImageTable, StateDecodeError> RestoredImageTable::build(ImagePool& pool,
                                                                              ImageTableState const& table)
{
    // Check everything before anything is built.
    auto ids = std::set<uint32_t> {};
    for (auto const& image: table.images)
        if (!ids.insert(unbox<uint32_t>(image.id)).second
            || !isConsistentPixmap(image.format, image.size, image.data.size()))
            return StateFailure { StateDecodeError::InvalidValue };
    auto const hasImage = [&](ImageId id) {
        return ids.contains(unbox<uint32_t>(id));
    };
    if (!std::ranges::all_of(table.rasterized, hasImage, &RasterizedImageState::image)
        || !std::ranges::all_of(table.names, hasImage, &NamedImageState::image)
        || !std::ranges::all_of(table.fragments, [&](ImageFragmentState const& fragment) {
               return fragment.rasterized < table.rasterized.size();
           }))
        return StateFailure { StateDecodeError::InvalidValue };

    auto images = std::unordered_map<uint32_t, std::shared_ptr<Image const>> {};
    for (auto const& image: table.images)
        images.emplace(unbox<uint32_t>(image.id),
                       pool.adopt(image.id, image.format, image.size, Image::Data(image.data)));

    auto rasterized = std::vector<std::shared_ptr<RasterizedImage const>> {};
    rasterized.reserve(table.rasterized.size());
    for (auto const& raster: table.rasterized)
        rasterized.push_back(std::make_shared<RasterizedImage>(images.at(unbox<uint32_t>(raster.image)),
                                                               raster.alignment,
                                                               raster.resize,
                                                               raster.defaultColor,
                                                               raster.cellSpan,
                                                               raster.cellSize,
                                                               raster.layer,
                                                               raster.offset,
                                                               raster.subSize));

    auto fragments = std::vector<std::shared_ptr<ImageFragment>> {};
    fragments.reserve(table.fragments.size());
    for (auto const& fragment: table.fragments)
        fragments.push_back(
            std::make_shared<ImageFragment>(rasterized.at(fragment.rasterized), fragment.offset));
    return RestoredImageTable { table, std::move(images), std::move(fragments) };
}

std::shared_ptr<ImageFragment> RestoredImageTable::fragment(uint64_t id) const
{
    return id < _fragments.size() ? _fragments[static_cast<size_t>(id)] : nullptr;
}

std::shared_ptr<Image const> RestoredImageTable::image(ImageId id) const
{
    auto const it = _images.find(unbox<uint32_t>(id));
    return it != _images.end() ? it->second : nullptr;
}

void RestoredImageTable::commit(ImagePool& pool) const
{
    pool.continueNumberingAt(_table->nextImageId);
    for (auto const& named: _table->names)
        pool.link(named.name, _images.at(unbox<uint32_t>(named.image)));
}
// }}}

// {{{ Screen
KittyScreenState Screen::captureKittyState(ImageTableBuilder& images) const
{
    auto state = KittyScreenState {};
    for (auto const& [kittyId, image]: _kittyImages)
        state.images.emplace_back(kittyId, images.addImage(image));
    std::ranges::sort(state.images);
    for (auto const& [kittyId, animation]: _kittyAnimations)
    {
        auto captured = KittyAnimationState { .id = kittyId,
                                              .frames = {},
                                              .currentFrame = animation.currentFrame,
                                              .state = animation.state,
                                              .loopCount = animation.loopCount };
        for (auto const& frame: animation.frames)
            captured.frames.push_back(
                { .image = images.addImage(frame.image), .gapMilliseconds = frame.gapMilliseconds });
        state.animations.push_back(std::move(captured));
    }
    std::ranges::sort(state.animations, {}, &KittyAnimationState::id);
    if (_kittyChunkedCommand)
        state.chunked = KittyChunkState { .payload = _kittyChunkedPayload, .command = *_kittyChunkedCommand };
    return state;
}

std::expected<void, StateDecodeError> Screen::validateKittyState(KittyScreenState const& state,
                                                                 ImageFragmentResolver const& resolver)
{
    auto const isKnown = [&](ImageId id) {
        return resolver.image(id) != nullptr;
    };
    // Each id once: the tables are maps, and a second entry would be dropped silently.
    auto const isUnique = [](auto&& ids) {
        auto seen = std::set<uint32_t> {};
        return std::ranges::all_of(ids, [&](uint32_t id) { return seen.insert(id).second; });
    };
    if (!isUnique(state.images | std::views::keys)
        || !isUnique(state.animations | std::views::transform(&KittyAnimationState::id))
        || !std::ranges::all_of(state.images | std::views::values, isKnown)
        || !std::ranges::all_of(state.animations, [&](KittyAnimationState const& animation) {
               return std::ranges::all_of(animation.frames, isKnown, &KittyFrameState::image);
           }))
        return StateFailure { StateDecodeError::InvalidValue };
    return {};
}

std::expected<void, StateDecodeError> Screen::restoreKittyState(KittyScreenState const& state,
                                                                ImageFragmentResolver const& resolver)
{
    if (auto const valid = validateKittyState(state, resolver); !valid)
        return valid;
    _kittyImages.clear();
    for (auto const& [kittyId, id]: state.images)
        _kittyImages.insert_or_assign(kittyId, resolver.image(id));
    _kittyAnimations.clear();
    for (auto const& captured: state.animations)
    {
        auto animation = KittyAnimation { .frames = {},
                                          .currentFrame = captured.currentFrame,
                                          .state = captured.state,
                                          .loopCount = captured.loopCount };
        for (auto const& frame: captured.frames)
            animation.frames.push_back(
                { .image = resolver.image(frame.image), .gapMilliseconds = frame.gapMilliseconds });
        _kittyAnimations.insert_or_assign(captured.id, std::move(animation));
    }
    if (state.chunked)
    {
        _kittyChunkedPayload = state.chunked->payload;
        _kittyChunkedCommand = state.chunked->command;
    }
    else
    {
        _kittyChunkedPayload.clear();
        _kittyChunkedCommand.reset();
    }
    return {};
}
// }}}

} // namespace vtbackend
