// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Image.h>

#include <crispy/size.h>

#include <charconv>
#include <string_view>
#include <system_error>

namespace contour
{

/// Pure argument parsers for the `contour cat` image subcommand, extracted from ContourApp so the
/// (dependency-free) string→value mapping is unit-testable without dispatching the CLI. Each returns
/// a documented default for an unrecognized/empty value — `cat` never rejects an argument, it falls
/// back — so these encode the CLI's tolerance policy in one place.

/// Parses a size string in the format "WxH" (e.g. "80x24"); returns {0,0} on any invalid format.
[[nodiscard]] inline crispy::size parseCatSize(std::string_view text) noexcept
{
    auto const pos = text.find('x');
    if (pos == std::string_view::npos || pos == 0 || pos == text.size() - 1)
        return crispy::size {};

    auto const widthStr = text.substr(0, pos);
    auto const heightStr = text.substr(pos + 1);

    int width = 0;
    int height = 0;
    auto const [we, werr] = std::from_chars(widthStr.data(), widthStr.data() + widthStr.size(), width);
    auto const [he, herr] = std::from_chars(heightStr.data(), heightStr.data() + heightStr.size(), height);
    if (werr != std::errc {} || herr != std::errc {})
        return crispy::size {};

    return crispy::size { .width = width, .height = height };
}

/// Parses an image alignment string; unrecognized values default to MiddleCenter.
[[nodiscard]] inline vtbackend::ImageAlignment parseCatAlignment(std::string_view text) noexcept
{
    if (text == "top-start")
        return vtbackend::ImageAlignment::TopStart;
    if (text == "top-center")
        return vtbackend::ImageAlignment::TopCenter;
    if (text == "top-end")
        return vtbackend::ImageAlignment::TopEnd;
    if (text == "middle-start")
        return vtbackend::ImageAlignment::MiddleStart;
    if (text == "middle-center" || text == "center")
        return vtbackend::ImageAlignment::MiddleCenter;
    if (text == "middle-end")
        return vtbackend::ImageAlignment::MiddleEnd;
    if (text == "bottom-start")
        return vtbackend::ImageAlignment::BottomStart;
    if (text == "bottom-center")
        return vtbackend::ImageAlignment::BottomCenter;
    if (text == "bottom-end")
        return vtbackend::ImageAlignment::BottomEnd;
    return vtbackend::ImageAlignment::MiddleCenter;
}

/// Parses an image resize policy string; unrecognized values default to ResizeToFit.
[[nodiscard]] inline vtbackend::ImageResize parseCatResize(std::string_view text) noexcept
{
    if (text == "no" || text == "none")
        return vtbackend::ImageResize::NoResize;
    if (text == "fit")
        return vtbackend::ImageResize::ResizeToFit;
    if (text == "fill")
        return vtbackend::ImageResize::ResizeToFill;
    if (text == "stretch")
        return vtbackend::ImageResize::StretchToFill;
    return vtbackend::ImageResize::ResizeToFit;
}

/// Parses an image compositing-layer value ("0"/"below", "1"/"replace", "2"/"above"); defaults to 1.
[[nodiscard]] inline int parseCatLayer(std::string_view text) noexcept
{
    if (text == "0" || text == "below")
        return 0;
    if (text == "1" || text == "replace")
        return 1;
    if (text == "2" || text == "above")
        return 2;
    return 1; // default: replace
}

} // namespace contour
