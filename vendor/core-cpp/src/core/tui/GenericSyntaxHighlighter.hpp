// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::tui
{

struct Theme;

/// @brief Category of a syntax token for colorization.
enum class HighlightCategory : std::uint8_t
{
    Default,      ///< Unclassified text.
    Keyword,      ///< Language keywords (if, else, for, etc.).
    Number,       ///< Numeric literals.
    String,       ///< String literals and delimiters.
    Operator,     ///< Operators (+, -, |>, etc.).
    Variable,     ///< Variables ($VAR, ${VAR}).
    Constructor,  ///< Constructors and type constructors.
    Comment,      ///< Comments (line and block).
    Type,         ///< Type names and annotations.
    Punctuation,  ///< Brackets, semicolons, commas.
    Function,     ///< Built-in functions and commands.
    Preprocessor, ///< Preprocessor directives (#include, #define).
};

/// @brief The languages core::tui highlights out of the box.
///
/// An application's own language is not one of these: it is taught to a
/// SyntaxHighlighterRegistry, which hands back an id of its own from the range that begins at
/// FirstRegisteredLanguageId.
enum class LanguageId : std::uint8_t
{
    None,       ///< No language — no highlighting applied.
    Cpp,        ///< C and C++.
    CMake,      ///< CMake build scripts.
    Python,     ///< Python.
    Bash,       ///< Bash/sh shell scripts.
    Markdown,   ///< Markdown.
    Json,       ///< JSON.
    Yaml,       ///< YAML.
    GitDiff,    ///< Git diff output.
    Assembly,   ///< x86 assembly (Intel and AT&T syntax).
    PowerShell, ///< PowerShell scripts.
    Cmd,        ///< Windows CMD / batch scripts.
    Xml,        ///< XML and XML-based dialects (.props, .csproj, .xaml, .svg, …).
    Ini,        ///< INI / .editorconfig configuration files.

    Last, ///< Not a language: the number of languages above it, so a table or a test can cover
          ///< every one of them without restating the list. Never detected, never highlighted,
          ///< never returned by a registry. **A new language goes above it, never below**, and
          ///< below FirstRegisteredLanguageId, which is where registered ids start. Appending one
          ///< *after* `Last` is the mistake to avoid, and the guards catch it only in part, which
          ///< is measured rather than assumed: `-Wswitch` demands a case in highlightBuiltin(),
          ///< so the enumerator cannot be added silently, but once that case exists the build is
          ///< clean — builtinLanguageTableIsIndexedByLanguageId() compares this table's size
          ///< against `Last`, and appending after `Last` moves neither, so it fires only once the
          ///< table row is added, which is to say once the mistake is half corrected. Everything
          ///< that walks `[0, Last)` — BuiltinLanguageTable, name(), the three golden tests —
          ///< misses it entirely. `GenericSyntaxHighlighter_test.cpp`'s
          ///< "no_language_hides_above_Last" is what refuses a hidden language that *works*.
};

/// @brief The first id SyntaxHighlighterRegistry::registerLanguage() issues.
///
/// Values below it are the enumerators of LanguageId; values from here up are registered
/// languages, numbered densely in registration order. The gap is deliberate: a built-in can be
/// appended without renumbering anything a registry has issued.
inline constexpr auto FirstRegisteredLanguageId = std::uint8_t { 128 };

/// @brief Whether @p language was issued by a SyntaxHighlighterRegistry.
///
/// @warning **A registered id belongs to the registry that issued it.** Ids are dense from
///          FirstRegisteredLanguageId in registration order and carry nothing that identifies
///          their registry, so passing one to a different registry is a precondition violation:
///          if that registry has issued an id in the same position, the line is highlighted as
///          *its* language, silently and wrongly, and only if it has not is the result plain
///          text. This is the contract `std::vector::iterator` has with its container, and it is
///          the price of keeping LanguageId as one small trivially copyable type. A program that
///          holds one registry — which is the shape this is designed for — cannot hit it.
///
/// @note The built-in half is not issued by anybody and *is* portable: an id below
///       FirstRegisteredLanguageId means the same language everywhere, in any registry and in
///       none, and may be passed freely.
[[nodiscard]] constexpr auto isRegisteredLanguage(LanguageId language) noexcept -> bool
{
    return std::to_underlying(language) >= FirstRegisteredLanguageId;
}

/// @brief State carried across lines for multi-line constructs.
enum class HighlightState : std::uint8_t
{
    Normal,            ///< Normal scanning state.
    BlockComment,      ///< Inside a /* ... */ block comment.
    RawString,         ///< Inside a C++ R"(...)" raw string.
    TripleQuoteString, ///< Inside a Python """ or ''' triple-quoted string.
    XmlComment,        ///< Inside an XML <!-- ... --> comment.
    PsBlockComment,    ///< Inside a PowerShell <# ... #> block comment.
};

/// @brief Per-character highlight category for a line.
using HighlightMap = std::vector<HighlightCategory>;

class SyntaxHighlighterRegistry;

/// @brief Detects language from a file extension (e.g. ".cpp", ".py").
/// @param ext The file extension including the leading dot.
/// @param registry Languages registered beside the built-in ones, or nullptr for built-ins only.
/// @return The detected language, or LanguageId::None.
[[nodiscard]] constexpr auto detectLanguageFromExtension(std::string_view ext,
                                                         SyntaxHighlighterRegistry const* registry = nullptr)
    -> LanguageId;

/// @brief Detects language from a markdown fence tag (e.g. "cpp", "python").
/// @param tag The fence language tag (without the backticks).
/// @param registry Languages registered beside the built-in ones, or nullptr for built-ins only.
/// @return The detected language, or LanguageId::None.
[[nodiscard]] constexpr auto detectLanguageFromFenceTag(std::string_view tag,
                                                        SyntaxHighlighterRegistry const* registry = nullptr)
    -> LanguageId;

/// @brief Detects language from a full file path by extracting the extension.
/// @param filePath The file path.
/// @param registry Languages registered beside the built-in ones, or nullptr for built-ins only.
/// @return The detected language, or LanguageId::None.
[[nodiscard]] auto detectLanguageFromPath(std::string_view filePath,
                                          SyntaxHighlighterRegistry const* registry = nullptr) -> LanguageId;

/// @brief Highlights a single line of source code.
///
/// Performs a single-pass scan of the line, classifying each character position
/// into a HighlightCategory. Supports multi-line state (block comments, raw strings,
/// triple-quoted strings) via the state parameter.
///
/// @param line The source line to highlight.
/// @param language The language to use for highlighting rules.
/// @param state The current multi-line state (from previous line's output).
/// @param registry Languages registered beside the built-in ones, or nullptr for built-ins only.
/// @return A pair of (per-character highlight map, updated state for next line).
[[nodiscard]] auto highlightLine(std::string_view line,
                                 LanguageId language,
                                 HighlightState state = HighlightState::Normal,
                                 SyntaxHighlighterRegistry const* registry = nullptr)
    -> std::pair<HighlightMap, HighlightState>;

/// @brief Maps a highlight category to its color from the theme's syntax palette.
/// @param cat The highlight category.
/// @param theme The current theme.
/// @return The RGB color for the category.
[[nodiscard]] auto categoryColor(HighlightCategory cat, Theme const& theme) -> RgbColor;

/// @brief Renders a syntax-highlighted line to the terminal.
///
/// Writes the text as a sequence of colored spans, grouping consecutive characters
/// with the same highlight category into single writeText calls for efficiency.
///
/// @param output The terminal output to write to.
/// @param text The source text.
/// @param highlights Per-character highlight categories (must be same length as text).
/// @param baseStyle Base style to apply (highlight colors override the fg).
/// @param theme The current theme for color lookup.
void renderHighlightedLine(TerminalOutput& output,
                           std::string_view text,
                           HighlightMap const& highlights,
                           Style baseStyle,
                           Theme const& theme);

/// @brief Renders a syntax-highlighted line to a string with ANSI SGR color codes.
///
/// Produces a string with embedded true-color ANSI escape sequences
/// (\033[38;2;r;g;bm) for each colored span, terminated with a reset (\033[m).
///
/// @param text The source text.
/// @param highlights Per-character highlight categories (must be same length as text).
/// @param theme The current theme for color lookup.
/// @return A string containing the text with embedded ANSI color codes.
[[nodiscard]] auto renderHighlightedLineToString(std::string_view text,
                                                 HighlightMap const& highlights,
                                                 Theme const& theme) -> std::string;

/// @brief Callback type for external language highlighters.
using HighlightFunction =
    std::function<std::pair<HighlightMap, HighlightState>(std::string_view line, HighlightState state)>;

/// @brief A language an application teaches a SyntaxHighlighterRegistry.
struct LanguageDefinition
{
    std::string name;                    ///< Identifies the language; non-empty and not already taken.
    std::vector<std::string> extensions; ///< File extensions, each with its leading dot (".toy").
    std::vector<std::string> fenceTags;  ///< Markdown fence tags, lowercase, without backticks.
    HighlightFunction highlight;         ///< Highlights one line of this language; must be callable.
};

/// @brief What SyntaxHighlighterRegistry::registerLanguage() refused a definition for.
enum class LanguageRegistrationError : std::uint8_t
{
    EmptyName,       ///< The definition carried no name.
    NoHighlighter,   ///< The definition carried no highlight function.
    NameInUse,       ///< A built-in or already-registered language answers to that name.
    TokenInUse,      ///< A built-in or already-registered language claims that extension or tag.
    MalformedToken,  ///< An extension without its leading dot, or an empty extension or fence tag.
    CapacityReached, ///< Every id in the registered range has been handed out.
};

/// @brief Why a registration was refused, and what in the definition caused it.
///
/// The token is carried because the caller has several of them in one definition and cannot tell
/// from the code alone which extension or fence tag was already claimed.
struct LanguageRegistrationFailure
{
    LanguageRegistrationError error; ///< What was wrong.
    std::string token;               ///< The name, extension or fence tag at fault; empty if none applies.
};

/// @brief The languages an application registers, beside the built-in ones.
///
/// core::tui ships the languages of LanguageId and no others. An application that has its own
/// teaches it here — a name, the extensions and Markdown fence tags that select it, and the
/// function that highlights a line of it — and passes the registry to whatever renders it
/// (MarkdownRenderer, StyledText::fromMarkdown, the free detection and highlighting functions).
/// There is no process-wide registry: a registry is constructed, filled and injected, so two
/// parts of one program can hold different ones and a test never has to undo a registration.
///
/// A registry answers for the built-in languages too, so registering one does not cost an
/// application the ones core::tui ships.
///
/// @note Copyable and movable. A registered id belongs to the registry that issued it and must
///       not be passed to another one — see isRegisteredLanguage() for what that costs. Built-in
///       ids are portable everywhere.
class SyntaxHighlighterRegistry
{
  public:
    /// @brief Teaches this registry one language.
    ///
    /// Refuses a definition whose name, extension or fence tag is already claimed, by a built-in
    /// language or by one registered earlier, rather than shadowing what is there: replacing
    /// would repoint an id already handed out, and the holder of that id would then get a wrong
    /// answer that looks right. A refused definition changes nothing — the registry is left
    /// exactly as it was.
    ///
    /// @param definition The language to register.
    /// @return Its id, or why it was refused.
    [[nodiscard]] auto registerLanguage(LanguageDefinition definition)
        -> std::expected<LanguageId, LanguageRegistrationFailure>;

    /// @brief Returns the language registered or built in under @p name, or LanguageId::None.
    [[nodiscard]] auto find(std::string_view name) const noexcept -> LanguageId;

    /// @brief Returns the name of @p language, or an empty view if this registry does not know it.
    [[nodiscard]] auto name(LanguageId language) const noexcept -> std::string_view;

    /// @brief Returns how many languages were registered here (built-ins are not counted).
    [[nodiscard]] auto registeredCount() const noexcept -> std::size_t;

    /// @brief Detects a language from a file extension, built-in rows first.
    /// @param ext The file extension including the leading dot.
    [[nodiscard]] auto detectFromExtension(std::string_view ext) const noexcept -> LanguageId;

    /// @brief Detects a language from a Markdown fence tag, built-in rows first.
    /// @param tag The fence language tag, without the backticks.
    [[nodiscard]] auto detectFromFenceTag(std::string_view tag) const noexcept -> LanguageId;

    /// @brief Detects a language from a file path, by well-known name and then by extension.
    ///
    /// @note Only the built-in languages are selected by a well-known file name: an application
    ///       knows what its own configuration file is called and names the language itself.
    [[nodiscard]] auto detectFromPath(std::string_view filePath) const -> LanguageId;

    /// @brief Highlights one line, through a registered highlighter or a built-in one.
    ///
    /// @p language must be a built-in id or one **this** registry issued. A registered id from
    /// another registry resolves to whatever sits in the same position here, which is a different
    /// language, and only an id past the end of this registry yields plain text; see
    /// isRegisteredLanguage().
    ///
    /// @param line The source line to highlight.
    /// @param language The language to use for highlighting rules.
    /// @param state The current multi-line state (from the previous line's output).
    /// @return A pair of (per-character highlight map, updated state for the next line).
    [[nodiscard]] auto highlightLine(std::string_view line,
                                     LanguageId language,
                                     HighlightState state = HighlightState::Normal) const
        -> std::pair<HighlightMap, HighlightState>;

  private:
    /// @brief Finds the registered language whose @p tokens list holds @p token.
    ///
    /// The extension list and the fence-tag list are searched the same way, so the member to
    /// search is a parameter rather than the difference between two copies of this loop.
    [[nodiscard]] auto findByToken(std::string_view token,
                                   std::vector<std::string> LanguageDefinition::* tokens) const noexcept
        -> LanguageId;

    /// @brief Registered languages, in registration order; index i holds id FirstRegisteredLanguageId + i.
    std::vector<LanguageDefinition> _languages;
};

// --- Data-driven language detection tables ---

/// @brief Associates a textual token (file extension or fence tag) with a language.
struct LanguageToken
{
    std::string_view token; ///< The extension (with leading dot) or fence tag to match.
    LanguageId language;    ///< The language this token maps to.
};

/// @brief Associates a built-in language with the name it answers to.
struct LanguageName
{
    LanguageId language;   ///< The built-in language.
    std::string_view name; ///< Its canonical name, lowercase.
};

/// @brief Every built-in language, indexed by its enumerator.
///
/// This is the list of languages core::tui ships. It gives each one a name, so a registration
/// that would shadow one is refused by name as well as by token, and so a diagnostic can say
/// which language a line was highlighted as.
inline constexpr auto BuiltinLanguageTable = std::to_array<LanguageName>({
    { .language = LanguageId::None, .name = "none" },
    { .language = LanguageId::Cpp, .name = "cpp" },
    { .language = LanguageId::CMake, .name = "cmake" },
    { .language = LanguageId::Python, .name = "python" },
    { .language = LanguageId::Bash, .name = "bash" },
    { .language = LanguageId::Markdown, .name = "markdown" },
    { .language = LanguageId::Json, .name = "json" },
    { .language = LanguageId::Yaml, .name = "yaml" },
    { .language = LanguageId::GitDiff, .name = "gitdiff" },
    { .language = LanguageId::Assembly, .name = "assembly" },
    { .language = LanguageId::PowerShell, .name = "powershell" },
    { .language = LanguageId::Cmd, .name = "cmd" },
    { .language = LanguageId::Xml, .name = "xml" },
    { .language = LanguageId::Ini, .name = "ini" },
});

/// @brief Checks that every row of BuiltinLanguageTable sits at its own enumerator's index.
///
/// Anchoring the table's extent on LanguageId::Last rather than on the last language by name is
/// what makes appending an enumerator and forgetting its row a compile error instead of a
/// silent read past the end.
[[nodiscard]] consteval auto builtinLanguageTableIsIndexedByLanguageId() -> bool
{
    if (BuiltinLanguageTable.size() != static_cast<std::size_t>(LanguageId::Last))
        return false;
    for (auto const i: std::views::iota(std::size_t { 0 }, BuiltinLanguageTable.size()))
        if (static_cast<std::size_t>(std::to_underlying(BuiltinLanguageTable[i].language)) != i)
            return false;
    return true;
}

static_assert(builtinLanguageTableIsIndexedByLanguageId(),
              "BuiltinLanguageTable must hold one row per LanguageId, in enumerator order.");

/// @brief File extension → language table (extensions include the leading dot).
///
/// Each row maps one extension to one language; add a row to teach the highlighter a
/// new extension. Several distinct languages are intentionally folded onto a shared
/// highlighter (e.g. JS/TS/Rust/Go reuse the C-family highlighter, TOML reuses YAML).
inline constexpr auto ExtensionLanguageTable = std::to_array<LanguageToken>({
    // C / C++ and C-family languages sharing the C++ highlighter.
    { .token = ".cpp", .language = LanguageId::Cpp },
    { .token = ".cxx", .language = LanguageId::Cpp },
    { .token = ".cc", .language = LanguageId::Cpp },
    { .token = ".c", .language = LanguageId::Cpp },
    { .token = ".hpp", .language = LanguageId::Cpp },
    { .token = ".hxx", .language = LanguageId::Cpp },
    { .token = ".hh", .language = LanguageId::Cpp },
    { .token = ".h", .language = LanguageId::Cpp },
    { .token = ".ipp", .language = LanguageId::Cpp },
    { .token = ".js", .language = LanguageId::Cpp },
    { .token = ".jsx", .language = LanguageId::Cpp },
    { .token = ".ts", .language = LanguageId::Cpp },
    { .token = ".tsx", .language = LanguageId::Cpp },
    { .token = ".rs", .language = LanguageId::Cpp },
    { .token = ".go", .language = LanguageId::Cpp },
    { .token = ".java", .language = LanguageId::Cpp },
    { .token = ".kt", .language = LanguageId::Cpp },
    { .token = ".cs", .language = LanguageId::Cpp },
    // Python and Python-like languages.
    { .token = ".py", .language = LanguageId::Python },
    { .token = ".pyw", .language = LanguageId::Python },
    { .token = ".pyi", .language = LanguageId::Python },
    { .token = ".rb", .language = LanguageId::Python },
    { .token = ".lua", .language = LanguageId::Python },
    // Shell.
    { .token = ".sh", .language = LanguageId::Bash },
    { .token = ".bash", .language = LanguageId::Bash },
    { .token = ".zsh", .language = LanguageId::Bash },
    // Data / config.
    { .token = ".json", .language = LanguageId::Json },
    { .token = ".jsonl", .language = LanguageId::Json },
    { .token = ".yml", .language = LanguageId::Yaml },
    { .token = ".yaml", .language = LanguageId::Yaml },
    { .token = ".toml", .language = LanguageId::Yaml },
    { .token = ".ini", .language = LanguageId::Ini },
    // Markup / build / diff.
    { .token = ".md", .language = LanguageId::Markdown },
    { .token = ".markdown", .language = LanguageId::Markdown },
    { .token = ".cmake", .language = LanguageId::CMake },
    { .token = ".diff", .language = LanguageId::GitDiff },
    { .token = ".patch", .language = LanguageId::GitDiff },
    { .token = ".asm", .language = LanguageId::Assembly },
    { .token = ".s", .language = LanguageId::Assembly },
    { .token = ".S", .language = LanguageId::Assembly },
    { .token = ".nasm", .language = LanguageId::Assembly },
    // PowerShell.
    { .token = ".ps1", .language = LanguageId::PowerShell },
    { .token = ".psm1", .language = LanguageId::PowerShell },
    { .token = ".psd1", .language = LanguageId::PowerShell },
    { .token = ".ps1xml", .language = LanguageId::PowerShell },
    // Windows CMD / batch.
    { .token = ".cmd", .language = LanguageId::Cmd },
    { .token = ".bat", .language = LanguageId::Cmd },
    // XML and XML-based dialects.
    { .token = ".xml", .language = LanguageId::Xml },
    { .token = ".props", .language = LanguageId::Xml },
    { .token = ".csproj", .language = LanguageId::Xml },
    { .token = ".targets", .language = LanguageId::Xml },
    { .token = ".vcxproj", .language = LanguageId::Xml },
    { .token = ".nuspec", .language = LanguageId::Xml },
    { .token = ".resx", .language = LanguageId::Xml },
    { .token = ".xaml", .language = LanguageId::Xml },
    { .token = ".svg", .language = LanguageId::Xml },
    { .token = ".plist", .language = LanguageId::Xml },
    { .token = ".xsd", .language = LanguageId::Xml },
    { .token = ".wxs", .language = LanguageId::Xml },
});

/// @brief Well-known file name → language table (matched against the whole basename).
///
/// The third of the module's three built-in tables, and the one detectLanguageFromPath() consults
/// first. These files carry no conventional extension yet have a well-defined format: the
/// .clang-format and .clang-tidy tool configs are YAML, and .editorconfig is INI. Build files
/// (Makefile, Dockerfile, …) are folded onto their closest existing highlighter.
///
/// A row belongs here only when the name is well known beyond any one project. An application's
/// own dotfile is that application's business: it knows what its configuration file is called and
/// passes the language to highlightLine() rather than asking this table to guess. One consumer's
/// own `-format` dotfile was a row here and is not one any more, for that reason.
///
/// @note A registered language never claims a file name — only extensions and fence tags — so
///       this table is exactly what core::tui ships and nothing else. It is public so that the
///       same golden test that pins the other two pins this one: it was the table that carried a
///       consumer's name, and it was the one nothing guarded.
inline constexpr auto FilenameLanguageTable = std::to_array<LanguageToken>({
    { .token = "CMakeLists.txt", .language = LanguageId::CMake },
    { .token = "Makefile", .language = LanguageId::Bash },
    { .token = "makefile", .language = LanguageId::Bash },
    { .token = "GNUmakefile", .language = LanguageId::Bash },
    { .token = "Dockerfile", .language = LanguageId::Bash },
    { .token = ".clang-format", .language = LanguageId::Yaml },
    { .token = ".clang-tidy", .language = LanguageId::Yaml },
    { .token = ".editorconfig", .language = LanguageId::Ini },
});

/// @brief Markdown fence tag → language table (lowercase tags, without backticks).
inline constexpr auto FenceTagLanguageTable = std::to_array<LanguageToken>({
    { .token = "cpp", .language = LanguageId::Cpp },
    { .token = "c++", .language = LanguageId::Cpp },
    { .token = "cxx", .language = LanguageId::Cpp },
    { .token = "c", .language = LanguageId::Cpp },
    { .token = "h", .language = LanguageId::Cpp },
    { .token = "hpp", .language = LanguageId::Cpp },
    { .token = "javascript", .language = LanguageId::Cpp },
    { .token = "js", .language = LanguageId::Cpp },
    { .token = "jsx", .language = LanguageId::Cpp },
    { .token = "typescript", .language = LanguageId::Cpp },
    { .token = "ts", .language = LanguageId::Cpp },
    { .token = "tsx", .language = LanguageId::Cpp },
    { .token = "rust", .language = LanguageId::Cpp },
    { .token = "rs", .language = LanguageId::Cpp },
    { .token = "go", .language = LanguageId::Cpp },
    { .token = "golang", .language = LanguageId::Cpp },
    { .token = "java", .language = LanguageId::Cpp },
    { .token = "kotlin", .language = LanguageId::Cpp },
    { .token = "kt", .language = LanguageId::Cpp },
    { .token = "csharp", .language = LanguageId::Cpp },
    { .token = "cs", .language = LanguageId::Cpp },
    { .token = "c#", .language = LanguageId::Cpp },
    { .token = "python", .language = LanguageId::Python },
    { .token = "py", .language = LanguageId::Python },
    { .token = "ruby", .language = LanguageId::Python },
    { .token = "rb", .language = LanguageId::Python },
    { .token = "lua", .language = LanguageId::Python },
    { .token = "bash", .language = LanguageId::Bash },
    { .token = "sh", .language = LanguageId::Bash },
    { .token = "shell", .language = LanguageId::Bash },
    { .token = "zsh", .language = LanguageId::Bash },
    { .token = "dockerfile", .language = LanguageId::Bash },
    { .token = "docker", .language = LanguageId::Bash },
    { .token = "json", .language = LanguageId::Json },
    { .token = "jsonl", .language = LanguageId::Json },
    { .token = "yaml", .language = LanguageId::Yaml },
    { .token = "yml", .language = LanguageId::Yaml },
    { .token = "toml", .language = LanguageId::Yaml },
    { .token = "markdown", .language = LanguageId::Markdown },
    { .token = "md", .language = LanguageId::Markdown },
    { .token = "cmake", .language = LanguageId::CMake },
    { .token = "diff", .language = LanguageId::GitDiff },
    { .token = "patch", .language = LanguageId::GitDiff },
    { .token = "asm", .language = LanguageId::Assembly },
    { .token = "assembly", .language = LanguageId::Assembly },
    { .token = "nasm", .language = LanguageId::Assembly },
    { .token = "x86", .language = LanguageId::Assembly },
    { .token = "x86asm", .language = LanguageId::Assembly },
    { .token = "intel", .language = LanguageId::Assembly },
    { .token = "att", .language = LanguageId::Assembly },
    { .token = "gas", .language = LanguageId::Assembly },
    { .token = "powershell", .language = LanguageId::PowerShell },
    { .token = "pwsh", .language = LanguageId::PowerShell },
    { .token = "ps", .language = LanguageId::PowerShell },
    { .token = "ps1", .language = LanguageId::PowerShell },
    { .token = "posh", .language = LanguageId::PowerShell },
    { .token = "bat", .language = LanguageId::Cmd },
    { .token = "batch", .language = LanguageId::Cmd },
    { .token = "cmd", .language = LanguageId::Cmd },
    { .token = "dosbatch", .language = LanguageId::Cmd },
    { .token = "dos", .language = LanguageId::Cmd },
    { .token = "xml", .language = LanguageId::Xml },
    { .token = "xaml", .language = LanguageId::Xml },
    { .token = "svg", .language = LanguageId::Xml },
    { .token = "html", .language = LanguageId::Xml },
    { .token = "xhtml", .language = LanguageId::Xml },
    { .token = "ini", .language = LanguageId::Ini },
    { .token = "editorconfig", .language = LanguageId::Ini },
    { .token = "dosini", .language = LanguageId::Ini },
});

/// @brief Looks up a token in a language table, returning the mapped language or None.
/// @param table The token→language table to search.
/// @param token The token to look up (exact match).
/// @return The mapped language, or LanguageId::None if the token is not present.
template <std::size_t N>
[[nodiscard]] constexpr auto lookupLanguage(std::array<LanguageToken, N> const& table, std::string_view token)
    -> LanguageId
{
    auto const it = std::ranges::find(table, token, &LanguageToken::token);
    return it != table.end() ? it->language : LanguageId::None;
}

// --- Inline constexpr implementations ---

constexpr auto detectLanguageFromExtension(std::string_view ext, SyntaxHighlighterRegistry const* registry)
    -> LanguageId
{
    if (registry != nullptr)
        return registry->detectFromExtension(ext);
    return lookupLanguage(ExtensionLanguageTable, ext);
}

constexpr auto detectLanguageFromFenceTag(std::string_view tag, SyntaxHighlighterRegistry const* registry)
    -> LanguageId
{
    if (registry != nullptr)
        return registry->detectFromFenceTag(tag);
    return lookupLanguage(FenceTagLanguageTable, tag);
}

} // namespace core::tui
