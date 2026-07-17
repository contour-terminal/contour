// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <optional>

namespace vtbackend
{

/// Decides what a maximize or full-screen request does to a window's size, and how to undo it.
///
/// This is a *frontend's* problem, not a terminal's: putting a window back to the size it had before it
/// was maximized is a window manager's job, and a terminal engine has no window manager. But every
/// frontend needs the same arithmetic -- the GUI, the conformance harness, and the mock a unit test
/// drives -- so the decision lives here, dependency-free and testable on its own, rather than being
/// written out once per frontend and drifting apart.
///
/// A window is in exactly one of three states: normal, maximized (along one or both axes), or
/// full-screen. Entering either of the latter two remembers the size to come back to; leaving restores
/// it. Entering one while already in the other simply re-targets, and the remembered size still names
/// the window's last *normal* size.
class WindowSizeStack
{
  public:
    /// @param how     What the application asked for.
    /// @param current The window's size now, in cells.
    /// @param screen  The size of the screen the window is on, in cells.
    /// @return The size the window should take, or std::nullopt to leave it as it is.
    [[nodiscard]] constexpr std::optional<PageSize> maximize(WindowMaximize how,
                                                             PageSize current,
                                                             PageSize screen) noexcept
    {
        if (how == WindowMaximize::Restore)
            return restore();

        remember(current);

        switch (how)
        {
            case WindowMaximize::Both: return screen;
            case WindowMaximize::Vertically:
                return PageSize { .lines = screen.lines, .columns = current.columns };
            case WindowMaximize::Horizontally:
                return PageSize { .lines = current.lines, .columns = screen.columns };
            case WindowMaximize::Restore: break; // handled above
        }

        return std::nullopt;
    }

    /// @param how     What the application asked for.
    /// @param current The window's size now, in cells.
    /// @param screen  The size of the screen the window is on, in cells.
    /// @return The size the window should take, or std::nullopt to leave it as it is.
    [[nodiscard]] constexpr std::optional<PageSize> fullScreen(WindowFullScreen how,
                                                               PageSize current,
                                                               PageSize screen) noexcept
    {
        auto const enter = how == WindowFullScreen::Enter || (how == WindowFullScreen::Toggle && !enlarged());

        if (!enter)
            return restore();

        remember(current);
        return screen;
    }

    /// @return Whether the window has been maximized or made full-screen, and so has a size to go back to.
    [[nodiscard]] constexpr bool enlarged() const noexcept { return _normalSize.has_value(); }

  private:
    /// Remembers @p current as the size to come back to -- but only the first time, so that maximizing
    /// an already-maximized window does not make the screen's size the one it "came from".
    constexpr void remember(PageSize current) noexcept
    {
        if (!_normalSize.has_value())
            _normalSize = current;
    }

    /// @return The size the window came from, or std::nullopt if it was never enlarged.
    [[nodiscard]] constexpr std::optional<PageSize> restore() noexcept
    {
        auto const size = _normalSize;
        _normalSize = std::nullopt;
        return size;
    }

    /// The window's size before it was maximized or made full-screen.
    std::optional<PageSize> _normalSize;
};

} // namespace vtbackend
