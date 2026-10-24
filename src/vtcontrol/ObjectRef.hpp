// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <vtcontrol/EnumTable.hpp>
#include <vtcontrol/Uuid.hpp>

namespace vtcontrol
{

/// The kinds of object an id can name. A block has its own reference type (`BlockRef`).
enum class ObjectKind : uint8_t
{
    Window = 0,
    Tab,
    Pane,
};

template <>
struct EnumTable<ObjectKind>
{
    static constexpr std::array Rows {
        EnumName { ObjectKind::Window, "window" },
        EnumName { ObjectKind::Tab, "tab" },
        EnumName { ObjectKind::Pane, "pane" },
    };
};

/// A window, tab or pane, spelled `window:<uuid>`, `tab:<uuid>` or `pane:<uuid>`.
struct ObjectRef
{
    ObjectKind kind = ObjectKind::Pane; ///< What the id names.
    Uuid uuid {};                       ///< Which one.

    auto operator<=>(ObjectRef const&) const = default;
};

/// A command block, spelled `block:<pane-uuid>/<serial>`.
struct BlockRef
{
    Uuid pane;           ///< The pane the block lives in.
    uint64_t serial = 0; ///< See `blockSerial`.

    auto operator<=>(BlockRef const&) const = default;
};

/// @param ref The reference.
/// @return Its text, e.g. `pane:018f3c4e-9a1b-7c2d-8e3f-0123456789ab`.
[[nodiscard]] std::string toString(ObjectRef const& ref);

/// @param text The text of a reference.
/// @return The reference, or nothing when `text` is not a `window:`, `tab:` or `pane:` id.
[[nodiscard]] std::optional<ObjectRef> parseObjectRef(std::string_view text);

/// @param ref The reference.
/// @return Its text, e.g. `block:018f3c4e-9a1b-7c2d-8e3f-0123456789ab/4294967338`.
[[nodiscard]] std::string toString(BlockRef const& ref);

/// @param text The text of a block reference.
/// @return The reference, or nothing when `text` is not one.
[[nodiscard]] std::optional<BlockRef> parseBlockRef(std::string_view text);

/// @param epoch The block store's id epoch, which changes whenever its ids restart.
/// @param id The block's id within the epoch.
/// @return The serial that is unique over the pane's whole life: `(epoch << 32) | id`.
[[nodiscard]] constexpr uint64_t blockSerial(uint64_t epoch, uint32_t id) noexcept
{
    return (epoch << 32) | id;
}

} // namespace vtcontrol
