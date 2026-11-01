// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace vtworkspace
{

/// The tree a tab's panes are to be laid out as, its leaves named by the sessions the tab already shows:
/// what SessionModel::arrangeTab() rearranges a tab into, as a host mirroring another model's tab
/// describes it.
struct PaneArrangement
{
    SplitState split = SplitState::None;   //!< None for a leaf; the axis of the split otherwise.
    double ratio = 0.5;                    //!< A split's first child's space share.
    SessionId session {};                  //!< The session a leaf shows.
    std::vector<PaneArrangement> children; //!< A split's two children, first then second; none for a leaf.
};

/// Why SessionModel::arrangeTab refused. On any of these the model is untouched and no event fired.
enum class ArrangeError : uint8_t
{
    UnknownTab = 0, //!< No tab has the id.
    Malformed,      //!< A split without exactly two children, or a leaf with any.
    OtherSessions,  //!< The leaves name other sessions than the tab shows, or one of them twice.
};

/// @return A short, stable description of @p error, for logs.
[[nodiscard]] constexpr std::string_view describe(ArrangeError error) noexcept
{
    switch (error)
    {
        case ArrangeError::UnknownTab: return "unknown tab";
        case ArrangeError::Malformed: return "malformed arrangement";
        case ArrangeError::OtherSessions: return "arrangement names other sessions than the tab shows";
    }
    return "unknown arrange error";
}

} // namespace vtworkspace
