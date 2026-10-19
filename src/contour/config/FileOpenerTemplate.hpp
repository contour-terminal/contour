// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkTypes.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::config
{

/// Why a file_opener command cannot be used.
enum class FileOpenerError : uint8_t
{
    Empty = 0,               ///< No program.
    UnknownPlaceholder,      ///< A `{name}` other than {path}, {line} and {column}.
    UnterminatedPlaceholder, ///< A `{` with no `}` after it.
    /// The program is empty, or holds a placeholder: a pane would run the shell instead, or read printed text
    /// as a command line again.
    ProgramNotWrittenOut,
};

/// What a file_opener problem is, for a log line or a settings field.
/// @param error The problem.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] std::string_view describe(FileOpenerError error) noexcept;

/// The file_opener command line, split into arguments once and expanded per file (§7.4).
///
/// The command is read as a configured program is read (vtpty::readAsCommandLine()): on POSIX as the words a
/// POSIX shell reads, on Windows the program as CreateProcess() finds it and the rest by
/// detail::splitWrittenArguments(). The placeholders are then filled inside each argument, so a file name
/// holding a space or a `;` stays one argument. No shell is involved.
class FileOpenerTemplate
{
  public:
    /// Splits @p command into its program and arguments, and finds the placeholders in them.
    /// @param command The configured command line.
    /// @return The template, or why it cannot be used.
    [[nodiscard]] static std::expected<FileOpenerTemplate, FileOpenerError> parse(std::string_view command);

    /// The program and its arguments for one file.
    /// @param path The file, as it is to be opened.
    /// @param location Where in it; {line} and {column} are 1 where the printed path had no location.
    /// @return The program first; never empty.
    [[nodiscard]] std::vector<std::string> expand(std::string_view path,
                                                  std::optional<vtbackend::LinkLocation> location) const;

  private:
    /// What fills one piece of an argument.
    enum class Slot : uint8_t
    {
        Literal = 0, ///< The text as written.
        Path,        ///< {path}
        Line,        ///< {line}
        Column,      ///< {column}
    };

    /// One run of an argument: literal text, or a placeholder.
    struct Piece
    {
        Slot slot = Slot::Literal;
        std::string literal; ///< For Slot::Literal.
    };

    /// What parse() built: there is no template without a program.
    /// @param arguments The program first, then its arguments, each as the pieces it is made of.
    explicit FileOpenerTemplate(std::vector<std::vector<Piece>> arguments) noexcept:
        _arguments { std::move(arguments) }
    {
    }

    /// The arguments, the program first, each as the pieces it is made of.
    std::vector<std::vector<Piece>> _arguments;
};

namespace detail
{
    /// Splits the arguments of a Windows command line as written: whitespace separates them, double quotes
    /// group, and backslashes are literal -- the part of CreateProcess's rules a configured command needs.
    /// @param text What follows the program.
    /// @return The arguments.
    [[nodiscard]] std::vector<std::string> splitWrittenArguments(std::string_view text);
} // namespace detail

} // namespace contour::config
