// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <boxed-cpp/boxed.hpp>

namespace crispy
{

class console
{
  private:
    // clang-format off
    struct lines_tag {};
    struct columns_tag {};
    struct width_tag {};
    struct height_tag {};
    // clang-format on

  public:
    using lines = boxed::boxed<int, lines_tag>;
    using columns = boxed::boxed<int, columns_tag>;
    using width = boxed::boxed<int, width_tag>;
    using height = boxed::boxed<int, height_tag>;

    struct size
    {
        struct
        {
            columns columns;
            lines lines;
        } cells;
        struct
        {
            width width;
            height height;
        } pixels;
    };

    static console& get();

    console(int in, int out);

    template <typename... Args>
    void write(fmt::format_string<Args...> const& fmt, auto&&... args);

    void write(std::string_view text);

    [[nodiscard]] std::optional<size> window_size() const noexcept;

    enum class function_type
    {
        CSI,
        DCS,
        OSC,
        PM,
        APC,
    };

    using sequence_parameter = std::vector<unsigned>;
    using sequence_parameter_list = std::vector<sequence_parameter>;
    using sequence_handler = std::function<void(char /*leader*/,
                                                sequence_parameter_list const& /* parameters */,
                                                char /*intermediate*/,
                                                char /*final*/
                                                )>;

    void set_sequence_handler(sequence_handler handler);
    void reset_sequence_handler();

    std::string getline();

  private:
    struct Impl;
    std::unique_ptr<Impl, void (*)(Impl*)> _impl;
};

template <typename... Args>
inline void console::write(fmt::format_string<Args...> const& fmt, auto&&... args)
{
    write(fmt::format(fmt, std::forward<decltype(args)>(args)...));
}

} // namespace crispy
