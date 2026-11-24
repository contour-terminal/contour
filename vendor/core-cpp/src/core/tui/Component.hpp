// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/CursorShape.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/TuiCoreExport.hpp>
#include <core/tui/layout/Style.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace core::tui
{

class Canvas;
class Screen;

// Forward declaration from Tooltip.hpp
enum class TooltipContentType : std::uint8_t;

/// @brief Result of a hover query on a component.
///
/// Components return this from onHover() to provide tooltip content.
/// Supports plain text and markdown. Multi-line content causes the tooltip
/// to grow dynamically up to the Tooltip's configured max height (default 15),
/// after which scrolling is enabled automatically.
struct HoverResult
{
    std::string text;               ///< Tooltip content (plain text or markdown).
    Point position;                 ///< Component-relative anchor for tooltip placement.
    TooltipContentType contentType; ///< How to render the text.
};

/// Focus group identifier.
using FocusGroupId = std::string;

/// Default focus group name.
inline constexpr std::string_view DefaultFocusGroup = "default";

/// Result of event handling.
enum class EventResult : uint8_t
{
    Ignored,   ///< Event not handled, bubble to parent.
    Handled,   ///< Event handled, stop bubbling.
    FocusNext, ///< Request focus move to next component.
    FocusPrev, ///< Request focus move to previous component.
};

/// Layout parameters for a child component.
struct LayoutParams
{
    Rect area {};                                             ///< Position and size relative to parent.
    bool visible = true;                                      ///< Whether to render this component.
    int zIndex = 0;                                           ///< Stacking order (higher = on top).
    FocusGroupId focusGroup = std::string(DefaultFocusGroup); ///< Focus group this component belongs to.

    /// How the parent's layout pass sizes this child; absent, the child keeps the @c area it is given.
    /// Present, @c area is not read: the child keeps the area it had until the parent's pass places it.
    ///
    /// Only the item half is read -- @c grow, @c shrink, @c basis, @c minSize and @c maxSize -- and the
    /// child's own @c minSize() and @c preferredSize() hints join it: the larger of the two minima
    /// holds, and the preferred size is the basis when none is given. How the child places its own
    /// children is its own style, given at its construction.
    std::optional<layout::Style> flex = std::nullopt;
};

/// Base class for all TUI components.
///
/// Components form a tree structure with parent-child relationships.
/// Each component has an area (position and size relative to its parent),
/// visibility state, and focus group membership.
///
/// Subclasses must implement render() to draw themselves using a Canvas.
/// They can optionally override event handling and focus-related methods.
class CORE_TUI_CORE_API Component
{
  public:
    virtual ~Component();

    // Prevent copying (component identity matters for tree structure)
    Component(Component const&) = delete;
    Component& operator=(Component const&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;

    // --- Rendering ---

    /// Renders this component to the given canvas.
    /// The canvas's area is already set to this component's bounds.
    /// @param canvas The canvas to render to.
    virtual void render(Canvas& canvas) = 0;

    /// Returns the preferred size for this component.
    /// Used by layout algorithms.
    [[nodiscard]] virtual Size preferredSize() const { return { .width = 0, .height = 1 }; }

    /// Returns the minimum size this component can shrink to.
    [[nodiscard]] virtual Size minSize() const { return { .width = 0, .height = 0 }; }

    // --- Event Handling ---

    /// Processes an input event.
    /// Override to handle keyboard, mouse, or other events.
    /// @param event The input event to process.
    /// @return EventResult indicating whether the event was handled.
    [[nodiscard]] virtual EventResult onEvent(InputEvent const& event);

    /// Called when this component gains focus.
    virtual void onFocus() {}

    /// Called when this component loses focus.
    virtual void onBlur() {}

    /// @brief Called when the user hovers over this component after the hover delay.
    ///
    /// Override to provide tooltip content. Supports single-line and multi-line
    /// content in both plain text and markdown formats. Tooltips grow dynamically
    /// in height and enable scrolling when content exceeds the configured maximum.
    ///
    /// @param x Component-relative column (0-based).
    /// @param y Component-relative row (0-based).
    /// @return Tooltip info if hover content available, std::nullopt otherwise.
    [[nodiscard]] virtual std::optional<HoverResult> onHover(int x, int y);

    // --- Focus ---

    /// Returns true if this component can receive keyboard focus.
    [[nodiscard]] virtual bool focusable() const { return false; }

    /// Returns true if this component currently has focus within its group.
    [[nodiscard]] bool focused() const noexcept { return _focused; }

    /// Returns the cursor shape to use when this component is focused.
    /// Default is CursorShape::Default, which lets the terminal decide.
    [[nodiscard]] virtual CursorShape cursorShape() const { return CursorShape::Default; }

    // --- Visibility ---

    /// Sets whether this component is visible.
    /// Invisible components are not rendered and don't receive events.
    void setVisible(bool visible);

    /// Returns true if this component is visible.
    /// Virtual so derived components (e.g. popups) can refine visibility while
    /// remaining correct when invoked through a Component base pointer.
    [[nodiscard]] virtual bool visible() const noexcept { return _layout.visible; }

    // --- Layout ---

    /// Sets the area (position and size) of this component relative to its parent.
    ///
    /// A change of size runs onResize() and then the layout pass over this component's children. A child
    /// added with a @c LayoutParams::flex item has its area set by its parent's layout pass, which
    /// overwrites an area set here.
    void setArea(Rect area);

    /// Returns how this component places the children it lays out; fixed at construction.
    [[nodiscard]] layout::Style const& layoutStyle() const noexcept { return _style; }

    /// Sets the area of every visible child added with a @c LayoutParams::flex item, from
    /// layout::solve() over this component's own size and its layoutStyle().
    ///
    /// Runs on its own whenever this component's size changes and whenever such a child is added,
    /// removed, shown or hidden. A caller runs it after a child's size hints changed. Children added
    /// without a flex item are left where they are.
    void layoutChildren();

    /// Tells this component's parent that its preferredSize() or minSize() may have changed, so that the
    /// parent lays its children out again: at the next frame, before it is drawn, when the component is on
    /// a screen, or at the parent's next layout pass otherwise. Many calls before then cost one pass.
    void hintsChanged();

    /// Whether this component's children must be laid out again because a child's hints changed.
    [[nodiscard]] bool layoutPending() const noexcept { return _layoutState == LayoutState::HintsChanged; }

    /// Returns the area of this component relative to its parent.
    [[nodiscard]] Rect area() const noexcept { return _layout.area; }

    /// Returns the absolute screen bounds of this component.
    /// Only valid after the component has been rendered.
    [[nodiscard]] Rect screenBounds() const noexcept { return _screenBounds; }

    /// Sets the absolute screen bounds of this component.
    /// Used by Screen during rendering, and for off-screen component rendering.
    void setScreenBounds(Rect bounds);

    /// Sets the z-index (stacking order) of this component.
    void setZIndex(int z);

    /// Returns the z-index of this component.
    [[nodiscard]] int zIndex() const noexcept { return _layout.zIndex; }

    // --- Hierarchy ---

    /// Adds a child component with the given layout parameters.
    /// The child's parent is set to this component.
    /// @param child The child component to add.
    /// @param params Layout parameters for the child.
    void addChild(Component& child, LayoutParams params = {});

    /// Removes a child component.
    /// The child's parent is set to nullptr.
    /// @param child The child component to remove.
    void removeChild(Component& child);

    /// Removes all child components.
    void clearChildren();

    /// Returns the children of this component.
    [[nodiscard]] std::span<Component* const> children() const noexcept;

    /// Returns the parent of this component, or nullptr if none.
    [[nodiscard]] Component* parent() const noexcept { return _parent; }

    // --- Focus Group ---

    /// Sets the focus group this component belongs to.
    void setFocusGroup(FocusGroupId group);

    /// Returns the focus group this component belongs to.
    [[nodiscard]] FocusGroupId const& focusGroup() const noexcept { return _layout.focusGroup; }

    // --- Screen Access ---

    /// Returns the screen this component is attached to, or nullptr.
    [[nodiscard]] Screen* screen() const noexcept { return _screen; }

  protected:
    /// Constructs a component that lays its flex children out in a row (layout::Style's defaults).
    Component() = default;

    /// Constructs a component that lays its flex children out by @p style.
    /// @param style How this component places the children added with a @c LayoutParams::flex item:
    ///        its container half (direction, wrap, justify, align, gap). Its item half is unused --
    ///        how a component is sized in its parent is the parent's @c LayoutParams.
    explicit Component(layout::Style style);

    /// Removes @p child, one of this component's children, and destroys it -- on a screen, once the tree
    /// change under way closes, so that a focused component in it still hears its onBlur(); off one, now.
    /// @param child The child, owned by the caller until now.
    void retireChild(std::unique_ptr<Component> child);

    /// Called when this component's size changes, before its children are laid out.
    /// @param size The new size.
    virtual void onResize([[maybe_unused]] Size size) {}

    /// Marks this component as needing a redraw.
    void invalidate();

  private:
    friend class Screen;

    /// Whether a child's size hints changed since this component last laid its children out.
    enum class LayoutState : std::uint8_t
    {
        Clean,        ///< Laid out from the hints it has.
        HintsChanged, ///< A flex child's hints changed; the next frame lays the children out again.
    };

    layout::Style _style {};
    LayoutState _layoutState = LayoutState::Clean;
    Component* _parent = nullptr;
    std::vector<Component*> _children;
    LayoutParams _layout;
    Rect _screenBounds; ///< Absolute screen position (computed during render)
    Screen* _screen = nullptr;
    bool _focused = false;
    Screen* _blurQueuedOn = nullptr; ///< The screen holding this component's deferred onBlur(), if any.

    /// Called by Screen to set the parent.
    void setParent(Component* parent);

    /// Called by Screen to attach/detach from the screen.
    void setScreen(Screen* screen);

    /// Called by Screen to update focus state.
    void setFocused(bool focused);

    /// Lays the parent's children out again when this child takes part in its layout.
    void relayoutParent();

    /// Runs onResize() and lays the children out when the area's size differs from @p previous.
    void areaChanged(Rect previous);
};

/// Implicit root component used by Screen.
/// This is a simple container that renders all its children.
class CORE_TUI_CORE_API RootComponent final: public Component
{
  public:
    void render(Canvas& canvas) override;
    [[nodiscard]] Size preferredSize() const override;
};

/// A container that draws nothing and lays its children out by a flexbox style.
///
/// The tree's inner nodes: a row of a list and its details, a column of panels and a key-hint line.
/// Children are either owned, through add(), or borrowed, through addChild() with a
/// @c LayoutParams::flex item; the box sets the areas of both on every resize.
class CORE_TUI_CORE_API FlexBox final: public Component
{
  public:
    /// @param style How the children are placed: direction, wrap, justify, align and gap.
    explicit FlexBox(layout::Style style);

    ~FlexBox() override;

    FlexBox(FlexBox const&) = delete;
    FlexBox& operator=(FlexBox const&) = delete;
    FlexBox(FlexBox&&) = delete;
    FlexBox& operator=(FlexBox&&) = delete;

    /// Draws nothing; the children draw themselves.
    void render(Canvas& canvas) override;

    /// Adds a child the box owns, after the children it has.
    /// @param child The child (must not be null).
    /// @param item How the box sizes it: the item half of a layout::Style.
    /// @return The child.
    Component& add(std::unique_ptr<Component> child, layout::Style item = {});

  private:
    std::vector<std::unique_ptr<Component>> _owned;
};

} // namespace core::tui
