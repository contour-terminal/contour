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

#include <crispy/LRUCache.h>
#include <crispy/boxed.h>

#include <list>
#include <memory>
#include <string>
#include <unordered_map>

namespace terminal
{

enum class hyperlink_state
{
    /// Default hyperlink state.
    Inactive,

    /// mouse or cursor is hovering this hyperlink
    Hover,

    /// mouse or cursor is hovering and has this item selected (e.g. via pressing Ctrl)
    // Active,
};

using uri = std::string;

struct hyperlink_info
{                       // TODO: rename to Hyperlink
    std::string userId; //!< application provied ID
    uri uri;
    mutable hyperlink_state state = hyperlink_state::Inactive;

    bool isLocal() const noexcept { return uri.size() >= 7 && uri.substr(0, 7) == "file://"; }

    [[nodiscard]] std::string_view host() const noexcept
    {
        if (auto const i = uri.find("://"); i != terminal::uri::npos)
            if (auto const j = uri.find('/', i + 3); j != terminal::uri::npos)
                return std::string_view { uri.data() + i + 3, j - i - 3 };

        return "";
    }

    [[nodiscard]] std::string_view path() const noexcept
    {
        if (auto const i = uri.find("://"); i != terminal::uri::npos)
            if (auto const j = uri.find('/', i + 3); j != terminal::uri::npos)
                return std::string_view { uri.data() + j };

        return "";
    }

    [[nodiscard]] std::string_view scheme() const noexcept
    {
        if (auto const i = uri.find("://"); i != terminal::uri::npos)
            return std::string_view { uri.data(), i };
        else
            return {};
    }
};

namespace detail
{
    struct hyperlink_tag
    {
    };
} // namespace detail
using hyperlink_id = crispy::boxed<uint16_t, detail::hyperlink_tag>;

bool is_local(hyperlink_info const& hyperlink);

using hyperlink_cache = crispy::LRUCache<hyperlink_id, std::shared_ptr<hyperlink_info>>;

struct hyperlink_storage
{
    hyperlink_cache cache { 1024 };
    hyperlink_id nextHyperlinkId = hyperlink_id(1);

    std::shared_ptr<hyperlink_info> hyperlinkById(hyperlink_id id) noexcept
    {
        if (!!id)
            if (auto* href = cache.try_get(id))
                return *href;
        return {};
    }

    std::shared_ptr<hyperlink_info const> hyperlinkById(hyperlink_id id) const noexcept
    {
        if (!!id)
            if (auto* href = cache.try_get(id))
                return *href;
        return {};
    }

    hyperlink_id hyperlinkIdByUserId(std::string const& id) noexcept
    {
        for (auto& href: cache)
        {
            if (href.value->userId == id)
            {
                cache.touch(href.key);
                return href.key;
            }
        }
        return hyperlink_id {};
    }
};

} // namespace terminal
