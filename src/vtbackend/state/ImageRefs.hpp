// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <cstdint>
#include <memory>

namespace vtbackend
{

/// Names the image fragments a capture meets: every cell that shows one refers to it by the id this hands
/// out, which is how a restore gives the cells that shared one ImageFragment the same object again.
class ImageFragmentNamer
{
  public:
    ImageFragmentNamer() = default;
    ImageFragmentNamer(ImageFragmentNamer const&) = default;
    ImageFragmentNamer(ImageFragmentNamer&&) = default;
    ImageFragmentNamer& operator=(ImageFragmentNamer const&) = default;
    ImageFragmentNamer& operator=(ImageFragmentNamer&&) = default;
    virtual ~ImageFragmentNamer() = default;

    /// Files @p fragment, and what it shows, unless it was filed already.
    /// @param fragment A fragment a cell shows.
    /// @return Its id within the capture: the same for every cell sharing the object.
    [[nodiscard]] virtual uint64_t fragmentId(std::shared_ptr<ImageFragment> const& fragment) = 0;
};

/// Turns the ids of a captured state back into the shared objects they name.
///
/// A restore asks it, so the one object a capture filed once is the one object every referrer gets back.
class ImageFragmentResolver
{
  public:
    ImageFragmentResolver() = default;
    ImageFragmentResolver(ImageFragmentResolver const&) = default;
    ImageFragmentResolver(ImageFragmentResolver&&) = default;
    ImageFragmentResolver& operator=(ImageFragmentResolver const&) = default;
    ImageFragmentResolver& operator=(ImageFragmentResolver&&) = default;
    virtual ~ImageFragmentResolver() = default;

    /// @param id A fragment id from the capture.
    /// @return The fragment, or null when the capture knows no such id.
    [[nodiscard]] virtual std::shared_ptr<ImageFragment> fragment(uint64_t id) const = 0;

    /// @param id An image id from the capture.
    /// @return The image, or null when the capture knows no such id.
    [[nodiscard]] virtual std::shared_ptr<Image const> image(ImageId id) const = 0;
};

/// The resolver for a state that holds no images: it refuses every reference, so a stream that names
/// one anyway is rejected rather than restored with a dangling cell.
class NoImagesResolver final: public ImageFragmentResolver
{
  public:
    [[nodiscard]] std::shared_ptr<ImageFragment> fragment(uint64_t /*id*/) const override { return nullptr; }

    [[nodiscard]] std::shared_ptr<Image const> image(ImageId /*id*/) const override { return nullptr; }
};

} // namespace vtbackend
