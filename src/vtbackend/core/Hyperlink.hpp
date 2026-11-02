// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/StateDecodeError.hpp>

#include <crispy/LRUCache.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

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

/// One link of the hyperlink cache, as plain data.
struct HyperlinkEntryState
{
    HyperlinkId id {};  ///< The id the cells carry.
    std::string userId; ///< @see HyperlinkInfo::userId
    URI uri;            ///< @see HyperlinkInfo::uri

    bool operator==(HyperlinkEntryState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.userId);
        visitor(3, self.uri);
    }
};

/// Everything a HyperlinkStorage holds, as plain data.
struct HyperlinkStorageState
{
    /// The cached links, most recently used first: the order that decides which one is evicted next.
    std::vector<HyperlinkEntryState> entries;
    HyperlinkId nextHyperlinkId = HyperlinkId(1); ///< The id the next new link takes.

    bool operator==(HyperlinkStorageState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.entries);
        visitor(2, self.nextHyperlinkId);
    }
};

struct HyperlinkStorage
{
    /// @return The links in recency order and the id counter. Defined in state/TerminalState.cpp.
    [[nodiscard]] HyperlinkStorageState captureState() const;

    /// Replaces the cache with @p state, relinking the entries so the least recently used one is the same.
    /// @return Nothing, or InvalidValue when @p state holds more links than the cache does, or one twice.
    ///         Defined in state/TerminalState.cpp.
    [[nodiscard]] std::expected<void, StateDecodeError> restoreState(HyperlinkStorageState const& state);

    /// @return Nothing, or InvalidValue when @p state holds more links than the cache keeps, a null id or one
    ///         id twice.
    [[nodiscard]] std::expected<void, StateDecodeError> validateState(
        HyperlinkStorageState const& state) const;

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
