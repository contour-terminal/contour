// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LRUCache.h>

#include <memory>
#include <string>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

enum class HyperlinkState : uint8_t
{
    /// Default hyperlink state.
    Inactive,

    /// mouse or cursor is hovering this hyperlink
    Hover,

    /// mouse or cursor is hovering and has this item selected (e.g. via pressing Ctrl)
    // Active,
};

using URI = std::string;

struct HyperlinkInfo
{                       // TODO: rename to Hyperlink
    std::string userId; //!< application provied ID
    URI uri;
    mutable HyperlinkState state = HyperlinkState::Inactive;

    bool isLocal() const noexcept { return uri.size() >= 7 && uri.starts_with("file://"); }

    [[nodiscard]] std::string_view host() const noexcept
    {
        if (auto const i = uri.find("://"); i != vtbackend::URI::npos)
            if (auto const j = uri.find('/', i + 3); j != vtbackend::URI::npos)
                return std::string_view { uri.data() + i + 3, j - i - 3 };

        return "";
    }

    [[nodiscard]] std::string_view path() const noexcept
    {
        if (auto const i = uri.find("://"); i != vtbackend::URI::npos)
            if (auto const j = uri.find('/', i + 3); j != vtbackend::URI::npos)
                return std::string_view { uri.data() + j };

        return "";
    }

    [[nodiscard]] std::string_view scheme() const noexcept
    {
        if (auto const i = uri.find("://"); i != vtbackend::URI::npos)
            return std::string_view { uri.data(), i };
        else
            return {};
    }
};

namespace detail
{
    struct HyperlinkTag
    {
    };
} // namespace detail
using HyperlinkId = boxed::boxed<uint16_t, detail::HyperlinkTag>;

bool is_local(HyperlinkInfo const& hyperlink);

using HyperlinkCache = crispy::lru_cache<HyperlinkId, std::shared_ptr<HyperlinkInfo>>;

struct HyperlinkStorage
{
    mutable HyperlinkCache cache { 1024 };
    HyperlinkId nextHyperlinkId = HyperlinkId(1);

    std::shared_ptr<HyperlinkInfo> hyperlinkById(HyperlinkId id) noexcept
    {
        if (!!id)
            if (auto* href = cache.try_get(id))
                return *href;
        return {};
    }

    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> hyperlinkById(HyperlinkId id) const noexcept
    {
        if (!!id)
            if (auto* href = cache.try_get(id))
                return *href;
        return {};
    }

    HyperlinkId hyperlinkIdByUserId(std::string const& id) const noexcept
    {
        for (auto& href: cache)
        {
            if (href.value->userId == id)
            {
                cache.touch(href.key);
                return href.key;
            }
        }
        return HyperlinkId {};
    }
};

} // namespace vtbackend
