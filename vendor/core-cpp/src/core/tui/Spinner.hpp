// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/TuiCoreExport.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

namespace core::tui
{

/// @brief Predefined spinner animation patterns.
enum class SpinnerType : std::uint8_t
{
    Dots,   ///< ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
    Line,   ///< - \ | /
    Circle, ///< ◐ ◓ ◑ ◒
    Arc,    ///< ◜ ◠ ◝ ◞ ◡ ◟
    Square, ///< ◰ ◳ ◲ ◱
    Arrow,  ///< ← ↖ ↑ ↗ → ↘ ↓ ↙
    Bounce, ///< ⠁ ⠂ ⠄ ⠂
    Pulse,  ///< ▁ ▂ ▃ ▄ ▅ ▆ ▇ █ ▇ ▆ ▅ ▄ ▃ ▂ ▁
    Clock,  ///< 🕐 🕑 🕒 ... (12 frames)
    Moon,   ///< 🌑 🌒 🌓 🌔 🌕 🌖 🌗 🌘
    Earth,  ///< 🌍 🌎 🌏
};

/// @brief Returns the frames for a given spinner type.
[[nodiscard]] CORE_TUI_CORE_API auto spinnerFrames(SpinnerType type) -> std::span<std::string_view const>;

/// @brief Returns the recommended frame interval for a spinner type in milliseconds.
[[nodiscard]] CORE_TUI_CORE_API auto spinnerInterval(SpinnerType type) -> std::chrono::milliseconds;

/// @brief A loading/progress spinner animation.
///
/// Tracks animation state and renders the current frame. Call tick() periodically
/// to advance the animation.
class CORE_TUI_CORE_API Spinner
{
  public:
    /// @brief Constructs a spinner with the given type.
    /// @param type The spinner animation pattern.
    /// @param clock The clock frames are timed on (not owned; must outlive this spinner).
    explicit Spinner(SpinnerType type = SpinnerType::Dots,
                     IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    Spinner(SpinnerType type, IClock const&& clock) = delete;

    /// @brief Constructs a spinner with custom frames.
    /// @param frames The frames to animate through.
    /// @param interval The time between frames.
    /// @param clock The clock frames are timed on (not owned; must outlive this spinner).
    Spinner(std::span<std::string_view const> frames,
            std::chrono::milliseconds interval,
            IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    Spinner(std::span<std::string_view const> frames,
            std::chrono::milliseconds interval,
            IClock const&& clock) = delete;

    /// @brief Advances the spinner to the next frame if enough time has passed.
    /// @return True if the frame changed and a re-render is needed.
    [[nodiscard]] auto tick() -> bool;

    /// @brief Returns the current frame as a string.
    [[nodiscard]] auto currentFrame() const noexcept -> std::string_view;

    /// @brief Returns the current frame index.
    [[nodiscard]] auto frameIndex() const noexcept -> std::size_t;

    /// @brief Returns the number of frames in the animation.
    [[nodiscard]] auto frameCount() const noexcept -> std::size_t;

    /// @brief Resets the spinner to the first frame.
    void reset();

    /// @brief Renders the spinner at the current cursor position.
    /// @param output The terminal output to render to.
    /// @param style The style to apply to the spinner.
    void render(TerminalOutput& output, Style const& style = {}) const;

    /// @brief Renders the spinner with a label.
    /// @param output The terminal output to render to.
    /// @param label The label to display after the spinner.
    /// @param spinnerStyle The style for the spinner.
    /// @param labelStyle The style for the label.
    void renderWithLabel(TerminalOutput& output,
                         std::string_view label,
                         Style const& spinnerStyle = {},
                         Style const& labelStyle = {}) const;

    /// @brief Sets the spinner type.
    /// @param type The new spinner type.
    void setType(SpinnerType type);

    /// @brief Returns the frame interval.
    [[nodiscard]] auto interval() const noexcept -> std::chrono::milliseconds;

    /// @brief Sets the frame interval.
    /// @param interval The new interval between frames.
    void setInterval(std::chrono::milliseconds interval);

  private:
    std::span<std::string_view const> _frames;
    std::chrono::milliseconds _interval;
    std::size_t _frameIndex = 0;
    IClock const* _clock; ///< Never null; a pointer so that a Spinner stays assignable.
    SteadyTimePoint _lastTick;
};

/// @brief A progress bar component.
class CORE_TUI_CORE_API ProgressBar
{
  public:
    /// @brief Constructs a progress bar.
    /// @param width The width of the bar in characters.
    explicit ProgressBar(int width = 20);

    /// @brief Sets the progress value.
    /// @param progress Progress from 0.0 to 1.0.
    void setProgress(float progress);

    /// @brief Returns the current progress.
    [[nodiscard]] auto progress() const noexcept -> float;

    /// @brief Sets the width of the progress bar.
    /// @param width The width in characters.
    void setWidth(int width);

    /// @brief Returns the width of the progress bar.
    [[nodiscard]] auto width() const noexcept -> int;

    /// @brief Renders the progress bar.
    /// @param output The terminal output to render to.
    /// @param filledStyle The style for the filled portion.
    /// @param emptyStyle The style for the empty portion.
    void render(TerminalOutput& output, Style const& filledStyle = {}, Style const& emptyStyle = {}) const;

    /// @brief Renders the progress bar with a percentage label.
    /// @param output The terminal output to render to.
    /// @param filledStyle The style for the filled portion.
    /// @param emptyStyle The style for the empty portion.
    /// @param labelStyle The style for the percentage label.
    void renderWithPercent(TerminalOutput& output,
                           Style const& filledStyle = {},
                           Style const& emptyStyle = {},
                           Style const& labelStyle = {}) const;

  private:
    float _progress = 0.0f;
    int _width = 20;

    // Characters for the progress bar
    static constexpr std::string_view FilledChar = "\u2588"; // █
    static constexpr std::string_view EmptyChar = "\u2591";  // ░
    static constexpr std::string_view PartialChars[] = {
        "\u2588", // █ (full)
        "\u2589", // ▉ (7/8)
        "\u258A", // ▊ (3/4)
        "\u258B", // ▋ (5/8)
        "\u258C", // ▌ (1/2)
        "\u258D", // ▍ (3/8)
        "\u258E", // ▎ (1/4)
        "\u258F", // ▏ (1/8)
    };
};

} // namespace core::tui
