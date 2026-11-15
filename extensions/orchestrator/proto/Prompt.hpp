// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/proto/Driver.hpp>
#include <orchestrator/proto/RunRefs.hpp>

namespace orchestrator::proto
{

/// Whether the prompt goes on reading lines.
enum class Continue : uint8_t
{
    Quit,   ///< `quit`, or the end of input.
    Yes,    ///< Read the next line.
    Failed, ///< The driver cannot go on (a store write failed), or the input could not be read.
};

/// The prototype's line prompt: one verb per line, each a row of its verb table.
class Prompt
{
  public:
    /// @param driver The driver the verbs feed.
    explicit Prompt(Driver& driver): _driver { driver } {}

    /// Reads what arrived on @p fd and carries out each complete line.
    /// @param fd A readable descriptor, such as standard input.
    /// @return Continue::Quit at the end of input or on `quit`; Continue::Failed when the driver cannot go
    /// on.
    [[nodiscard]] Continue readAvailable(int fd);

    /// Carries out one line.
    /// @param line A verb and its words, separated by spaces.
    /// @return Whether to read the next line.
    [[nodiscard]] Continue handle(std::string_view line);

    /// @return Every verb's usage, on one line.
    [[nodiscard]] static std::string usage();

  private:
    using Words = std::span<std::string_view const>;

    /// One verb: its name, the fewest words its line has (the verb included), and what carries it out.
    struct Verb
    {
        std::string_view name;
        std::size_t minWords = 1;
        std::string_view usage;
        Continue (Prompt::*handler)(Words) = nullptr;
    };

    /// One row `list` printed, which `dispatch <n>` names.
    struct ListRow
    {
        std::string repo;
        ItemRef item;
        std::string url;
    };

    [[nodiscard]] static std::span<Verb const> verbs() noexcept;

    [[nodiscard]] Continue feed(Input input);
    /// Resolves the run @p word names for @p action, says what will happen, and feeds the action; or says why
    /// no run was named, and feeds nothing.
    [[nodiscard]] Continue act(RunAction action, std::string_view word);
    [[nodiscard]] Continue list(Words words);
    [[nodiscard]] Continue dispatch(Words words);
    [[nodiscard]] Continue runs(Words words);
    [[nodiscard]] Continue end(Words words);
    [[nodiscard]] Continue retry(Words words);
    [[nodiscard]] Continue dismiss(Words words);
    [[nodiscard]] Continue refresh(Words words);
    [[nodiscard]] Continue quit(Words words);

    Driver& _driver;
    std::vector<ListRow> _rows;        ///< What the last `list` printed.
    std::vector<std::string> _runRows; ///< The run ids the last `runs` printed, row 1 first.
    std::string _buffer;               ///< Input read past the last complete line.
};

} // namespace orchestrator::proto
