// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <cstdint>

namespace vtbackend
{

/// What a detected link is, as far as switching its detection on or off is concerned.
enum class LinkCategory : uint8_t
{
    Url = 0,  ///< Anything a browser or a user pattern opens: URLs and custom patterns.
    FilePath, ///< A file on this machine: a path, or a `file:` URL.
};

/// Whether automatic link detection runs for one category on one screen.
enum class DetectLinks : uint8_t
{
    No = 0, ///< Printed text of this category is not scanned.
    Yes,    ///< Printed text of this category is scanned.
};

/// The detection switches of a single screen.
struct ScreenLinkDetection
{
    DetectLinks urls = DetectLinks::Yes;      ///< Whether URLs and custom patterns are detected.
    DetectLinks filePaths = DetectLinks::Yes; ///< Whether file paths and `file:` URLs are detected.

    /// Whether links of @p category are detected.
    /// @param category The category asked about.
    /// @return Whether detection is on for it.
    [[nodiscard]] constexpr bool detects(LinkCategory category) const noexcept
    {
        switch (category)
        {
            case LinkCategory::Url: return urls == DetectLinks::Yes;
            case LinkCategory::FilePath: return filePaths == DetectLinks::Yes;
        }
        return false;
    }

    /// Whether any category is detected.
    /// @return Whether at least one switch is on.
    [[nodiscard]] constexpr bool detectsAny() const noexcept
    {
        return urls == DetectLinks::Yes || filePaths == DetectLinks::Yes;
    }

    bool operator==(ScreenLinkDetection const&) const = default;
};

/// Automatic link detection, switched per screen: page 0 (the primary screen) against every other page.
struct LinkDetectionPolicy
{
    /// The switches of page 0.
    ScreenLinkDetection primaryScreen { .urls = DetectLinks::Yes, .filePaths = DetectLinks::Yes };
    /// The switches of every other page, the alternate screen among them.
    ScreenLinkDetection otherScreens { .urls = DetectLinks::Yes, .filePaths = DetectLinks::No };

    /// The switches that apply to a page.
    /// @param page The page index; 0 is the primary screen.
    /// @return The primary screen's switches for page 0, the other screens' for any other page.
    [[nodiscard]] constexpr ScreenLinkDetection forPage(PageIndex page) const noexcept
    {
        return page.value == 0 ? primaryScreen : otherScreens;
    }

    /// Whether any screen detects any category.
    /// @return Whether detection is on anywhere.
    [[nodiscard]] constexpr bool detectsAnywhere() const noexcept
    {
        return primaryScreen.detectsAny() || otherScreens.detectsAny();
    }

    bool operator==(LinkDetectionPolicy const&) const = default;
};

} // namespace vtbackend
