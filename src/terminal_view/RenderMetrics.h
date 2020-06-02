#pragma once
#include <string>

#include <fmt/format.h>

namespace terminal::view {

struct RenderMetrics {
    unsigned cellBackgroundRenderCount = 0;
    unsigned cachedText = 0; //!< number of text words that were rendered using the cache.
    unsigned shapedText = 0; //!< number of text segments that went through text shaping

    constexpr void clear() noexcept
    {
        cellBackgroundRenderCount = 0;
        shapedText = 0;
        cachedText = 0;
    }

    std::string to_string() const
    {
        return fmt::format(
            "background renders: {}, cached text: {}, shaped text: {}",
            cellBackgroundRenderCount,
            cachedText,
            shapedText
        );
    }
};

}
