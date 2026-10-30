// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test doubles for dressing the Settings window: a frame that records what it was asked, and the dressing
/// of a host whose window is not dressed at all.

#include <contour/platform/NativeWindowFrame.hpp>
#include <contour/platform/TitleBandMaterial.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/window/SettingsWindowHost.hpp>

#include <utility>
#include <vector>

namespace contour::test
{

/// A window frame that records every decoration applied instead of reaching the OS.
class RecordingWindowFrame final: public platform::NativeWindowFrame
{
  public:
    /// Appends @p decoration to applied.
    void apply(QWindow& /*window*/, platform::WindowDecoration decoration) override
    {
        applied.push_back(decoration);
    }

    void invalidateShadow(QWindow& /*window*/) override {}

    /// @return Zero: there are no OS controls over the chrome.
    [[nodiscard]] qreal nativeControlsInset(QWindow& /*window*/) const override { return 0.0; }

    std::vector<platform::WindowDecoration> applied; ///< Every decoration applied, in order.
};

/// The dressing of a Settings window nothing is drawn behind: the null frame and material, on a row that
/// asks for no backdrop. They hold no state, so one pair serves every host.
/// @return The dressing.
[[nodiscard]] inline window::SettingsWindowDressing undressedSettingsWindow()
{
    static auto frame = platform::NullWindowFrame {};
    static auto material = platform::NullTitleBandMaterial {};
    return window::SettingsWindowDressing {
        .frame = frame,
        .material = material,
        .inputs = [] { return window::SettingsBackdropInputs {}; },
        .decoration = platform::WindowDecoration::Server,
        .placement = {},
    };
}

/// The undressed Settings window, drawing its own frame beside a Linux header bar.
/// @param placement Where the desktop puts the window buttons.
/// @return The dressing.
[[nodiscard]] inline window::SettingsWindowDressing framelessSettingsWindow(
    config::WindowButtonLayout placement)
{
    auto dressing = undressedSettingsWindow();
    dressing.decoration = platform::WindowDecoration::Client;
    dressing.placement = [placement = std::move(placement)] {
        return placement;
    };
    return dressing;
}

/// A recording frame and material, and the backdrop inputs a case sets.
struct SettingsDressingRecorder
{
    RecordingWindowFrame frame;                     ///< The decorations applied.
    TitleBandRecord record;                         ///< The backdrop requests applied.
    RecordingTitleBandMaterial material { record }; ///< Records into @c record.
    window::SettingsBackdropInputs inputs;          ///< What the host reads as its backdrop's inputs.
    /// Who draws the window's frame; read once, when the host is built.
    platform::WindowDecoration decoration = platform::WindowDecoration::Server;
    config::WindowButtonLayout placement; ///< What the host reads as the desktop's button placement.

    /// @return The dressing over this recorder; it must outlive the host.
    [[nodiscard]] window::SettingsWindowDressing dressing()
    {
        return window::SettingsWindowDressing {
            .frame = frame,
            .material = material,
            .inputs = [this] { return inputs; },
            .decoration = decoration,
            .placement = [this] { return placement; },
        };
    }
};

} // namespace contour::test
