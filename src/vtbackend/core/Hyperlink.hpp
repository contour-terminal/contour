// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/LRUCache.hpp>

#include <memory>
#include <string>

#include <boxed-cpp/boxed.hpp>

namespace vtbackend
{

using URI = std::string;

struct HyperlinkInfo
{                       // TODO: rename to Hyperlink
    std::string userId; //!< application provided ID
    URI uri;

    /// Whether this link targets a file rather than a network resource. Which machine's file it is takes
    /// the host name to answer -- see @ref vtbackend::isLocalHost in FileUrl.hpp.
    [[nodiscard]] bool isLocal() const noexcept { return uri.starts_with("file://"); }
};

namespace detail
{
    struct HyperlinkTag
    {
    };
} // namespace detail
using HyperlinkId = boxed::boxed<uint16_t, detail::HyperlinkTag>;

using HyperlinkCache = crispy::LRUCache<HyperlinkId, std::shared_ptr<HyperlinkInfo>>;

struct HyperlinkStorage
{
    mutable HyperlinkCache cache { 1024 };
    HyperlinkId nextHyperlinkId = HyperlinkId(1);

    std::shared_ptr<HyperlinkInfo> hyperlinkById(HyperlinkId id) noexcept
    {
        if (!!id)
            if (auto* href = cache.tryGet(id))
                return *href;
        return {};
    }

    [[nodiscard]] std::shared_ptr<HyperlinkInfo const> hyperlinkById(HyperlinkId id) const noexcept
    {
        if (!!id)
            if (auto* href = cache.tryGet(id))
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
