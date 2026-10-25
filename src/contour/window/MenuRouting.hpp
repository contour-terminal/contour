// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/MenuTree.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>

#include <cstdint>

namespace contour::window
{

/// Where a menu ended up being shown.
enum class MenuRoute : uint8_t
{
    InScene = 0, ///< The caller shows it in the scene.
    Native,      ///< The native presenter showed it.
};

/// Routes a menu to the native presenter when the style asks for it, falling back to the scene.
///
/// The fallback is silent: the null presenter is the normal case off Apple.
/// @param presentation The style's menu presentation token.
/// @param presenter The native presenter; not called when @p presentation is InScene.
/// @param tree The menu to show.
/// @param anchor Where to open it.
/// @param onPicked Called with the picked row's action index, when the native presenter showed it.
/// @return Native when the presenter showed the menu, otherwise InScene.
[[nodiscard]] inline MenuRoute presentMenu(config::MenuPresentation presentation,
                                           platform::INativeMenuPresenter& presenter,
                                           command::MenuTree const& tree,
                                           platform::MenuAnchor const& anchor,
                                           platform::MenuPicked const& onPicked)
{
    switch (presentation)
    {
        case config::MenuPresentation::InScene: return MenuRoute::InScene;
        case config::MenuPresentation::Native:
            return presenter.present(tree, anchor, onPicked)
                .transform([] { return MenuRoute::Native; })
                .value_or(MenuRoute::InScene);
    }
    return MenuRoute::InScene;
}

} // namespace contour::window
