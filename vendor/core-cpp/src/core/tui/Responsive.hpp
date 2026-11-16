// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file Responsive.hpp
/// @brief A container that builds a different child layout for each band of sizes it is given.

#include <core/tui/Component.hpp>
#include <core/tui/Rect.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace core::tui
{

/// @brief One band of sizes and the layout built for it.
struct Breakpoint
{
    int minColumns = 0;                               ///< The narrowest width this layout is chosen at.
    int minRows = 0;                                  ///< The lowest height this layout is chosen at.
    std::function<std::unique_ptr<Component>()> make; ///< Builds the layout; empty or null shows nothing.
};

/// @brief A container that shows one of several layouts, chosen by the size it is given.
///
/// On every resize it picks the **last** breakpoint whose minima the size meets, so breakpoints are listed
/// narrowest first: `{0, 0}`, `{60, 0}` and `{100, 0}` give three column bands, < 60, 60–99 and ≥ 100. It
/// builds that breakpoint's layout only when the choice changes -- a resize within a band keeps the layout
/// it has, and lays it out again -- and destroys the one it replaces. The layout fills the container.
///
/// Nothing is built before the container has a size other than 0×0, and nothing is shown at a size no
/// breakpoint's minima are met at.
///
/// **Precondition:** a layout's hints must not size the container into another band. The container's
/// size comes from its parent, which reads the layout's preferredSize() and minSize(); a narrow layout
/// that prefers to be wide, while the wide one prefers to be narrow, would swap the two once per frame
/// for ever. The screen stops re-laying such a chain after a bounded number of frames
/// (Screen's MaxChainedLayoutSweeps), leaving whichever layout it reached, until a resize or a changed
/// hint asks again. A chain that settles, one Responsive's swap resizing another once, ends long before.
class Responsive final: public Component
{
  public:
    /// @param breakpoints The bands, narrowest first; fixed for the container's life.
    explicit Responsive(std::vector<Breakpoint> breakpoints);

    ~Responsive() override;

    Responsive(Responsive const&) = delete;
    Responsive& operator=(Responsive const&) = delete;
    Responsive(Responsive&&) = delete;
    Responsive& operator=(Responsive&&) = delete;

    /// Draws nothing; the chosen layout draws itself.
    void render(Canvas& canvas) override;

    /// The chosen layout's preferred size, or 0×0 when none is shown.
    [[nodiscard]] Size preferredSize() const override;

    /// The chosen layout's minimum size, or 0×0 when none is shown.
    [[nodiscard]] Size minSize() const override;

    /// @return The index of the breakpoint shown, or nothing before the first size or when none is met.
    [[nodiscard]] std::optional<std::size_t> breakpoint() const noexcept { return _chosen; }

    /// @return The layout shown, or null.
    [[nodiscard]] Component* current() const noexcept { return _current.get(); }

    /// @brief Picks the breakpoint a size chooses.
    /// @param breakpoints The bands, narrowest first.
    /// @param size The size to choose for.
    /// @return The index of the last breakpoint whose minColumns and minRows @p size meets, or nothing.
    [[nodiscard]] static std::optional<std::size_t> choose(std::vector<Breakpoint> const& breakpoints,
                                                           Size size) noexcept;

  protected:
    /// Rebuilds the layout when @p size chooses another breakpoint.
    void onResize(Size size) override;

  private:
    std::vector<Breakpoint> _breakpoints;
    std::optional<std::size_t> _chosen;
    std::unique_ptr<Component> _current;
};

} // namespace core::tui
