// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Box.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/List.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace core::tui
{

/// @brief Result of dialog interaction.
enum class DialogResult : std::uint8_t
{
    None,      ///< No action taken.
    Changed,   ///< Dialog state changed, re-render needed.
    Confirmed, ///< User confirmed selection.
    Cancelled, ///< User cancelled the dialog.
};

/// @brief Configuration for a selection dialog.
struct SelectDialogConfig
{
    std::string title;                         ///< Dialog title.
    std::vector<ListItem> items;               ///< Items to select from.
    int width = 50;                            ///< Dialog width (0 = auto).
    int maxHeight = 20;                        ///< Maximum dialog height.
    BorderStyle border = BorderStyle::Rounded; ///< Border style.
    Style borderStyle;                         ///< Style for the border.
    Style titleStyle;                          ///< Style for the title.
    bool dimBackground = true;                 ///< Dim the background behind the dialog.
    std::string_view confirmHint = "Enter";    ///< Hint text for confirm action.
    std::string_view cancelHint = "Esc";       ///< Hint text for cancel action.
};

/// @brief A modal selection dialog with a list of items.
///
/// Renders centered on screen with optional background dimming.
/// Supports keyboard navigation for list selection.
/// As a Component, SelectDialog can be added to a Screen's component tree.
class SelectDialog: public Component
{
  public:
    /// @brief Constructs a selection dialog with the given configuration.
    explicit SelectDialog(SelectDialogConfig config);
    ~SelectDialog() override = default;

    // --- Component Interface ---

    /// @brief Renders the dialog to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief Dialog can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size for the dialog.
    [[nodiscard]] Size preferredSize() const override;

    // --- SelectDialog-specific API ---

    /// @brief Processes an input event and returns the result.
    /// @param event The input event to process.
    /// @return The result of processing the event.
    [[nodiscard]] auto processEvent(InputEvent const& event) -> DialogResult;

    /// @brief Returns the selected item index.
    [[nodiscard]] auto selectedIndex() const noexcept -> std::size_t;

    /// @brief Returns the selected item, or nullopt if none.
    [[nodiscard]] auto selectedItem() const noexcept -> std::optional<ListItem const*>;

    /// @brief Sets a filter for the list items.
    /// @param filter The filter text.
    void setFilter(std::string_view filter);

    /// @brief Clears the filter.
    void clearFilter();

    /// @brief Updates the dialog configuration.
    void setConfig(SelectDialogConfig config);

  private:
    SelectDialogConfig _config;
    List _list;
};

/// @brief Configuration for a confirmation dialog.
struct ConfirmDialogConfig
{
    std::string title;                         ///< Dialog title.
    std::string message;                       ///< Message to display.
    std::string confirmLabel = "Yes";          ///< Confirm button label.
    std::string cancelLabel = "No";            ///< Cancel button label.
    bool defaultConfirm = false;               ///< Default to confirm button.
    int width = 40;                            ///< Dialog width.
    BorderStyle border = BorderStyle::Rounded; ///< Border style.
    Style borderStyle;                         ///< Style for the border.
    Style titleStyle;                          ///< Style for the title.
    Style messageStyle;                        ///< Style for the message.
    bool dimBackground = true;                 ///< Dim the background behind the dialog.
};

/// @brief A modal confirmation dialog with Yes/No buttons.
/// As a Component, ConfirmDialog can be added to a Screen's component tree.
class ConfirmDialog: public Component
{
  public:
    /// @brief Constructs a confirmation dialog.
    explicit ConfirmDialog(ConfirmDialogConfig config);
    ~ConfirmDialog() override = default;

    // --- Component Interface ---

    /// @brief Renders the dialog to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief Dialog can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size for the dialog.
    [[nodiscard]] Size preferredSize() const override;

    // --- ConfirmDialog-specific API ---

    /// @brief Processes an input event and returns the result.
    /// @param event The input event to process.
    /// @return The result of processing the event.
    [[nodiscard]] auto processEvent(InputEvent const& event) -> DialogResult;

    /// @brief Returns whether confirm is currently selected.
    [[nodiscard]] auto isConfirmSelected() const noexcept -> bool;

  private:
    ConfirmDialogConfig _config;
    bool _confirmSelected;
};

/// @brief Configuration for an input dialog.
struct InputDialogConfig
{
    std::string title;                         ///< Dialog title.
    std::string prompt;                        ///< Input prompt.
    std::string placeholder;                   ///< Placeholder text.
    std::string initialValue;                  ///< Initial input value.
    int width = 50;                            ///< Dialog width.
    BorderStyle border = BorderStyle::Rounded; ///< Border style.
    Style borderStyle;                         ///< Style for the border.
    Style titleStyle;                          ///< Style for the title.
    Style inputStyle;                          ///< Style for the input field.
    bool dimBackground = true;                 ///< Dim the background behind the dialog.
};

/// @brief A modal input dialog with a text field.
/// As a Component, InputDialog can be added to a Screen's component tree.
class InputDialog: public Component
{
  public:
    /// @brief Constructs an input dialog.
    explicit InputDialog(InputDialogConfig config);
    ~InputDialog() override = default;

    // --- Component Interface ---

    /// @brief Renders the dialog to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief Dialog can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size for the dialog.
    [[nodiscard]] Size preferredSize() const override;

    // --- InputDialog-specific API ---

    /// @brief Processes an input event and returns the result.
    /// @param event The input event to process.
    /// @return The result of processing the event.
    [[nodiscard]] auto processEvent(InputEvent const& event) -> DialogResult;

    /// @brief Returns the current input value.
    [[nodiscard]] auto value() const noexcept -> std::string_view;

    /// @brief Sets the input value.
    void setValue(std::string_view value);

  private:
    InputDialogConfig _config;
    std::string _value;
    std::size_t _cursor = 0;
};

} // namespace core::tui
