// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/LinkUrlTemplate.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// One pattern as configuration states it, before compilation: a `hint_patterns` entry.
struct LinkPatternSpec
{
    std::string name;                 ///< Identifier; a built-in's name overrides that built-in.
    std::optional<std::string> regex; ///< ECMAScript; absent keeps the built-in's (a name-only override).
    std::optional<LinkActivation> activation; ///< Absent: the built-in's, or HintOnly for a user pattern.
    /// `url:`, what a user pattern's match opens (LinkUrlTemplate); absent opens the match as printed. A
    /// built-in opens what it finds as it is, so one given to a built-in is ignored and reported.
    std::optional<std::string> urlTemplate {};

    bool operator==(LinkPatternSpec const&) const = default;
};

/// What is wrong with a pattern (§4.5 of the link-detection design).
enum class LinkPatternProblem : uint8_t
{
    InvalidRegex = 0,   ///< std::regex rejected it.
    MatchesEmpty,       ///< A clickable pattern that can match the empty string.
    NonAsciiInBracket,  ///< `[…]` holds a non-ASCII character, which std::regex matches byte-wise.
    UnknownBuiltin,     ///< A name-only entry names no built-in.
    BackReference,      ///< `\1`…`\9`: the engine patterns run on has no back-references (see LinkPatterns).
    InvalidUrlTemplate, ///< Its `url:` is no template (LinkUrlTemplateError).
    UnknownTemplateGroup, ///< Its `url:` names a capture group the regex does not have.
    TemplateOnBuiltin,    ///< A `url:` given to a built-in, which opens what it finds as it is.
    PathWithoutGroup,     ///< A path pattern without capture group 1, which names the path.
    /// A clickable pattern whose link takes its scheme from the printed text (no `url:`, or one that does not
    /// start with a scheme of its own): a match is a link only where that names a scheme Contour opens.
    PrintedScheme,
    /// A built-in made clickable whose matches name no scheme (githash, ipv4, ipv6): a built-in takes no
    /// `url:`, so its clickable has no effect.
    ClickableBuiltinWithoutScheme,
};

/// What a problem costs its pattern.
enum class LinkPatternSeverity : uint8_t
{
    Warning = 0, ///< The pattern is used as written.
    Error,       ///< The pattern is left out of the table.
};

/// One problem found while compiling the table: reported once at load, shown inline on the settings page.
struct LinkPatternDiagnostic
{
    std::string pattern;        ///< The pattern's name.
    LinkPatternProblem problem; ///< What is wrong.
    std::string detail;         ///< E.g. std::regex_error's text; may be empty.

    /// @return Whether the pattern is still used.
    [[nodiscard]] LinkPatternSeverity severity() const noexcept;
    /// @return One sentence naming the pattern and the problem.
    [[nodiscard]] std::string message() const;
};

/// A compiled pattern.
struct LinkPattern
{
    std::string name;
    LinkKind kind = LinkKind::Custom;
    LinkActivation activation = LinkActivation::HintOnly;
    std::string source; ///< The regex as written, for the settings page and diagnostics.
    std::regex regex;
    /// What a match opens, for a user pattern with `url:`; absent opens the match as printed.
    std::optional<LinkUrlTemplate> urlTemplate {};
};

/// The one pattern table: the built-ins merged with a profile's `hint_patterns`, compiled once and shared by
/// hint mode, passive detection, click resolution and the settings page's preview.
///
/// Patterns are ECMAScript, compiled for an engine whose stack does not grow with the line. libstdc++'s
/// default executor recursed once per matched character (until libstdc++ 16), which a long line turned into a
/// stack overflow, so with libstdc++ every pattern runs on its breadth-first executor instead
/// (`std::regex_constants::__polynomial`). libc++ matches iteratively; MSVC gives up with `error_stack`. What
/// the user documentation must say about custom patterns follows from that:
///  - **Back-references (`\1`) are not supported.** A pattern using one is refused at load
///    (LinkPatternProblem::BackReference), on every platform alike.
///  - **Write alternatives that do not overlap.** Where one alternative's match is a prefix of another's
///    at the same place (`foo|foobar`), libstdc++'s breadth-first executor takes the longer, ECMAScript
///    the first. The built-ins are written so that both agree.
class LinkPatterns
{
  public:
    /// Compiles the built-ins with @p entries applied in order. An entry named like a built-in overrides only
    /// the fields it gives; any other entry is appended. Never fails: a broken entry is left out and
    /// reported.
    /// @param entries The profile's patterns.
    /// @return The table and what was wrong with it.
    [[nodiscard]] static LinkPatterns compile(std::span<LinkPatternSpec const> entries);

    /// The built-ins alone, compiled once per process.
    [[nodiscard]] static std::shared_ptr<LinkPatterns const> builtins();

    /// The patterns, built-ins first, in table order.
    [[nodiscard]] std::span<LinkPattern const> patterns() const noexcept { return _patterns; }
    /// What was wrong: problems in table order, then entries naming no built-in.
    [[nodiscard]] std::span<LinkPatternDiagnostic const> diagnostics() const noexcept { return _diagnostics; }
    /// @return The pattern named @p name, or nullptr.
    [[nodiscard]] LinkPattern const* find(std::string_view name) const noexcept;
    /// @return Whether any pattern takes part in passive detection.
    [[nodiscard]] bool hasClickable() const noexcept;

  private:
    std::vector<LinkPattern> _patterns;
    std::vector<LinkPatternDiagnostic> _diagnostics;
};

/// The built-in patterns' names, in table order.
[[nodiscard]] std::span<std::string_view const> builtinLinkPatternNames() noexcept;

/// Whether @p name is a built-in pattern's: an entry by that name overrides the built-in, may leave out its
/// regex, and takes no `url:` template.
/// @param name A pattern's name.
/// @return Whether it names a built-in.
[[nodiscard]] bool isBuiltinLinkPatternName(std::string_view name) noexcept;

} // namespace vtbackend
