/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/boxed.h>
#include <crispy/LRUCache.h>
#include <string>
#include <unordered_map>
#include <list>
#include <memory>

namespace terminal {

enum class HyperlinkState {
    /// Default hyperlink state.
    Inactive,

    /// mouse or cursor is hovering this hyperlink
    Hover,

    /// mouse or cursor is hovering and has this item selected (e.g. via pressing Ctrl)
    //Active,
};

using URI = std::string;

struct HyperlinkInfo { // TODO: rename to Hyperlink
    std::string userId; //!< application provied ID
    URI uri;
    HyperlinkState state = HyperlinkState::Inactive;

    bool isLocal() const noexcept
    {
        return uri.size() >= 7 && uri.substr(0, 7) == "file://";
    }

    std::string_view host() const noexcept
    {
        if (auto const i = uri.find("://"); i != uri.npos)
            if (auto const j = uri.find('/', i + 3); j != uri.npos)
                return std::string_view{uri.data() + i + 3, j - i - 3};

        return "";
    }

    std::string_view path() const noexcept
    {
        if (auto const i = uri.find("://"); i != uri.npos)
            if (auto const j = uri.find('/', i + 3); j != uri.npos)
                return std::string_view{uri.data() + j};

        return "";
    }

    std::string_view scheme() const noexcept
    {
        if (auto const i = uri.find("://"); i != uri.npos)
            return std::string_view{uri.data(), i};
        else
            return {};
    }
};

namespace detail { struct HyperlinkTag{}; }
using HyperlinkId = crispy::boxed<uint16_t, detail::HyperlinkTag>;

bool is_local(HyperlinkInfo const& _hyperlink);

using HyperlinkCache = crispy::LRUCache<HyperlinkId, std::shared_ptr<HyperlinkInfo>>;

struct HyperlinkStorage
{
    HyperlinkCache cache{1024};
    HyperlinkId nextHyperlinkId = HyperlinkId(1);

    std::shared_ptr<HyperlinkInfo> hyperlinkById(HyperlinkId _id) noexcept
    {
        if (!!_id)
            if (auto href = cache.try_get(_id))
                return *href;
        return {};
    }

    std::shared_ptr<HyperlinkInfo const> hyperlinkById(HyperlinkId _id) const noexcept
    {
        if (!!_id)
            if (auto href = cache.try_get(_id))
                return *href;
        return {};
    }

    HyperlinkId hyperlinkIdByUserId(std::string const& _id) noexcept
    {
        for (auto& href: cache)
        {
            if (href.second->userId == _id)
            {
                cache.touch(href.first);
                return href.first;
            }
        }
        return HyperlinkId{};
    }
};

} // end namespace
