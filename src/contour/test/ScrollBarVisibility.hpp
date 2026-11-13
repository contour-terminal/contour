// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QMetaEnum>
#include <QtCore/QMetaObject>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace contour::test
{

/// The value of QQuickScrollBar's Policy enumerator @p name, read from @p bar's meta-object: Qt's own
/// numbering, not a copy of it (the Templates header that declares it is private).
/// @param bar  A ScrollBar.
/// @param name An enumerator: "AsNeeded", "AlwaysOff" or "AlwaysOn".
/// @return Its value; -1 where @p bar has no such enumerator.
[[nodiscard]] inline int scrollBarPolicy(QObject const& bar, char const* name)
{
    auto const* const meta = bar.metaObject();
    auto const index = meta->indexOfEnumerator("Policy");
    return index < 0 ? -1 : meta->enumerator(index).keyToValue(name);
}

/// Whether a scroll bar is shown, as one row of ScrollBarVisibilityRows expects it.
enum class ScrollBarShown : std::uint8_t
{
    No = 0,
    Yes,
};

/// One row: a scroll bar of @c policy showing @c size of its content is @c shown or not.
struct ScrollBarVisibilityRow
{
    char const* policy = ""; ///< A QQuickScrollBar::Policy enumerator, by name.
    double size = 1.0;       ///< The visible fraction of the content; 1 is all of it.
    ScrollBarShown shown = ScrollBarShown::No;
};

/// When a kit style's scroll bar shows: as AsNeeded promises, only while there is something to scroll.
inline constexpr auto ScrollBarVisibilityRows = std::array {
    ScrollBarVisibilityRow { .policy = "AsNeeded", .size = 1.0, .shown = ScrollBarShown::No },
    ScrollBarVisibilityRow { .policy = "AsNeeded", .size = 0.5, .shown = ScrollBarShown::Yes },
    // A content that fits to within a float's rounding fits (after the half above: QQuickScrollBar takes no
    // change it reads as fuzzily equal).
    ScrollBarVisibilityRow { .policy = "AsNeeded", .size = 1.0 - 1e-9, .shown = ScrollBarShown::No },
    ScrollBarVisibilityRow { .policy = "AlwaysOn", .size = 1.0, .shown = ScrollBarShown::Yes },
    ScrollBarVisibilityRow { .policy = "AlwaysOff", .size = 0.5, .shown = ScrollBarShown::No },
};

/// Holds an item in a scene for as long as it lives, and takes it out again however the scope ends: an item
/// outside any scene is never visible, whatever its own `visible` says.
class ItemInScene
{
  public:
    explicit ItemInScene(QQuickItem& item): _item { item } { _item.setParentItem(_window.contentItem()); }
    ItemInScene(ItemInScene const&) = delete;
    ItemInScene(ItemInScene&&) = delete;
    ItemInScene& operator=(ItemInScene const&) = delete;
    ItemInScene& operator=(ItemInScene&&) = delete;
    ~ItemInScene() { _item.setParentItem(nullptr); }

  private:
    QQuickWindow _window;
    QQuickItem& _item;
};

/// Checks @p bar, a style's ScrollBar, against every ScrollBarVisibilityRows row.
inline void checkScrollBarVisibility(QQuickItem& bar)
{
    ItemInScene const inScene { bar };
    bar.setHeight(100);
    for (auto const& row: ScrollBarVisibilityRows)
    {
        INFO("policy " << row.policy << ", size " << row.size);
        auto const policy = scrollBarPolicy(bar, row.policy);
        REQUIRE(policy >= 0);
        bar.setProperty("policy", policy);
        bar.setProperty("size", row.size);
        CHECK(bar.isVisible() == (row.shown == ScrollBarShown::Yes));
    }
}

} // namespace contour::test
