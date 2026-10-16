// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <string>
#include <utility>

namespace vtbackend
{

/// A command line as a shell reported it, or as it was read off the grid.
///
/// It arrives from whatever wrote to the PTY, so its bytes are untrusted: they may hold controls, bidi
/// overrides or ill-formed UTF-8. They are therefore private, and a reader asks for them by what it wants
/// them for -- display() to show, insertable() to type back through a bracketed paste, copyable() to put
/// on the clipboard -- so no consumer can put the raw bytes on screen by forgetting a call. raw() is for
/// what must carry them unchanged: the wire, a dump, comparison.
class CommandLineText
{
  public:
    /// No command line.
    CommandLineText() = default;

    /// @param raw The command line as reported or recovered, unchanged.
    explicit CommandLineText(std::string raw) noexcept: _raw { std::move(raw) } {}

    /// The bytes as they arrived, for whatever must carry them unchanged: the wire, a dump, comparison.
    /// Never for the screen or the PTY.
    /// @return The raw bytes.
    [[nodiscard]] std::string const& raw() const noexcept { return _raw; }

    /// @return The command line made safe to show (@see SanitizePurpose::Display).
    [[nodiscard]] std::string display() const { return sanitizeCommandLine(_raw, SanitizePurpose::Display); }

    /// @return The command line made safe to type back into a shell through a bracketed paste
    ///         (@see SanitizePurpose::Insert).
    [[nodiscard]] std::string insertable() const
    {
        return sanitizeCommandLine(_raw, SanitizePurpose::Insert);
    }

    /// The command line made safe to put on the clipboard, which may be pasted anywhere: one line, with
    /// nothing after it that would run it.
    /// @return The sanitised command line.
    /// @see SanitizePurpose::Clipboard
    [[nodiscard]] std::string copyable() const
    {
        return sanitizeCommandLine(_raw, SanitizePurpose::Clipboard);
    }

    /// @return Clipboard text preserving internal line breaks and tabs, without trailing whitespace.
    /// @see SanitizePurpose::ClipboardMultiline
    [[nodiscard]] std::string multilineCopyable() const
    {
        return sanitizeCommandLine(_raw, SanitizePurpose::ClipboardMultiline);
    }

    /// @return Whether there is no command line.
    [[nodiscard]] bool empty() const noexcept { return _raw.empty(); }

    bool operator==(CommandLineText const&) const = default;

  private:
    std::string _raw;
};

} // namespace vtbackend
