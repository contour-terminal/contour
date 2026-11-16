// SPDX-License-Identifier: Apache-2.0
#include <core/tui/GenericSyntaxHighlighter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace core::tui;
using Cat = HighlightCategory;

namespace
{
/// @brief Checks that all characters in range [start, start+count) have the expected category.
bool hasCategory(HighlightMap const& map, std::size_t start, std::size_t count, Cat expected)
{
    for (auto const i: std::views::iota(start, std::max(start, std::min(start + count, map.size()))))
        if (map[i] != expected)
            return false;
    return true;
}

/// @brief Checks that a specific position has the expected category.
bool categoryAt(HighlightMap const& map, std::size_t pos, Cat expected)
{
    return pos < map.size() && map[pos] == expected;
}
} // namespace

// =============================================================================
// Language detection
// =============================================================================

// Both table lookups are `constexpr` public API, and the registry parameter must not have cost
// them that: with no registry the body never reaches SyntaxHighlighterRegistry's non-constexpr
// members, and these assertions make every toolchain prove it rather than leaving it to whether
// one of them treats the function as never constant-evaluable.
static_assert(detectLanguageFromExtension(".cpp") == LanguageId::Cpp);
static_assert(detectLanguageFromExtension(".nope") == LanguageId::None);
static_assert(detectLanguageFromFenceTag("python") == LanguageId::Python);
static_assert(detectLanguageFromFenceTag("nope") == LanguageId::None);
static_assert(!isRegisteredLanguage(LanguageId::Cpp));
static_assert(isRegisteredLanguage(static_cast<LanguageId>(FirstRegisteredLanguageId)));

TEST_CASE("GenericSyntaxHighlighter.detectLanguageFromExtension", "[tui][highlight]")
{
    CHECK(detectLanguageFromExtension(".cpp") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".hpp") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".c") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".h") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".py") == LanguageId::Python);
    CHECK(detectLanguageFromExtension(".sh") == LanguageId::Bash);
    CHECK(detectLanguageFromExtension(".json") == LanguageId::Json);
    CHECK(detectLanguageFromExtension(".yml") == LanguageId::Yaml);
    CHECK(detectLanguageFromExtension(".yaml") == LanguageId::Yaml);
    CHECK(detectLanguageFromExtension(".md") == LanguageId::Markdown);
    CHECK(detectLanguageFromExtension(".cmake") == LanguageId::CMake);
    CHECK(detectLanguageFromExtension(".diff") == LanguageId::GitDiff);
    CHECK(detectLanguageFromExtension(".txt") == LanguageId::None);
    // Approximate mappings — C-family syntax
    CHECK(detectLanguageFromExtension(".js") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".jsx") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".ts") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".tsx") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".rs") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".go") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".java") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".kt") == LanguageId::Cpp);
    CHECK(detectLanguageFromExtension(".cs") == LanguageId::Cpp);
    // Ruby/Lua map to Python
    CHECK(detectLanguageFromExtension(".rb") == LanguageId::Python);
    CHECK(detectLanguageFromExtension(".lua") == LanguageId::Python);
    // TOML maps to YAML
    CHECK(detectLanguageFromExtension(".toml") == LanguageId::Yaml);
    // PowerShell
    CHECK(detectLanguageFromExtension(".ps1") == LanguageId::PowerShell);
    CHECK(detectLanguageFromExtension(".psm1") == LanguageId::PowerShell);
    CHECK(detectLanguageFromExtension(".psd1") == LanguageId::PowerShell);
    // Windows CMD / batch
    CHECK(detectLanguageFromExtension(".cmd") == LanguageId::Cmd);
    CHECK(detectLanguageFromExtension(".bat") == LanguageId::Cmd);
    // XML and XML-based dialects
    CHECK(detectLanguageFromExtension(".xml") == LanguageId::Xml);
    CHECK(detectLanguageFromExtension(".props") == LanguageId::Xml);
    CHECK(detectLanguageFromExtension(".csproj") == LanguageId::Xml);
    CHECK(detectLanguageFromExtension(".xaml") == LanguageId::Xml);
    CHECK(detectLanguageFromExtension(".svg") == LanguageId::Xml);
    // INI
    CHECK(detectLanguageFromExtension(".ini") == LanguageId::Ini);
}

TEST_CASE("GenericSyntaxHighlighter.detectLanguageFromFenceTag", "[tui][highlight]")
{
    CHECK(detectLanguageFromFenceTag("cpp") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("c++") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("python") == LanguageId::Python);
    CHECK(detectLanguageFromFenceTag("py") == LanguageId::Python);
    CHECK(detectLanguageFromFenceTag("bash") == LanguageId::Bash);
    CHECK(detectLanguageFromFenceTag("sh") == LanguageId::Bash);
    CHECK(detectLanguageFromFenceTag("shell") == LanguageId::Bash);
    CHECK(detectLanguageFromFenceTag("json") == LanguageId::Json);
    CHECK(detectLanguageFromFenceTag("yaml") == LanguageId::Yaml);
    CHECK(detectLanguageFromFenceTag("yml") == LanguageId::Yaml);
    CHECK(detectLanguageFromFenceTag("markdown") == LanguageId::Markdown);
    CHECK(detectLanguageFromFenceTag("cmake") == LanguageId::CMake);
    CHECK(detectLanguageFromFenceTag("diff") == LanguageId::GitDiff);
    CHECK(detectLanguageFromFenceTag("") == LanguageId::None);
    // Approximate mappings — C-family syntax
    CHECK(detectLanguageFromFenceTag("javascript") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("js") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("jsx") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("typescript") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("ts") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("tsx") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("rust") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("rs") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("go") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("golang") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("java") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("kotlin") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("kt") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("csharp") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("cs") == LanguageId::Cpp);
    CHECK(detectLanguageFromFenceTag("c#") == LanguageId::Cpp);
    // Ruby/Lua map to Python
    CHECK(detectLanguageFromFenceTag("ruby") == LanguageId::Python);
    CHECK(detectLanguageFromFenceTag("rb") == LanguageId::Python);
    CHECK(detectLanguageFromFenceTag("lua") == LanguageId::Python);
    // Dockerfile maps to Bash
    CHECK(detectLanguageFromFenceTag("dockerfile") == LanguageId::Bash);
    CHECK(detectLanguageFromFenceTag("docker") == LanguageId::Bash);
    // TOML maps to YAML
    CHECK(detectLanguageFromFenceTag("toml") == LanguageId::Yaml);
    // PowerShell
    CHECK(detectLanguageFromFenceTag("powershell") == LanguageId::PowerShell);
    CHECK(detectLanguageFromFenceTag("pwsh") == LanguageId::PowerShell);
    CHECK(detectLanguageFromFenceTag("ps1") == LanguageId::PowerShell);
    // Windows CMD / batch
    CHECK(detectLanguageFromFenceTag("bat") == LanguageId::Cmd);
    CHECK(detectLanguageFromFenceTag("batch") == LanguageId::Cmd);
    CHECK(detectLanguageFromFenceTag("cmd") == LanguageId::Cmd);
    // XML
    CHECK(detectLanguageFromFenceTag("xml") == LanguageId::Xml);
    CHECK(detectLanguageFromFenceTag("xaml") == LanguageId::Xml);
    // INI
    CHECK(detectLanguageFromFenceTag("ini") == LanguageId::Ini);
    CHECK(detectLanguageFromFenceTag("editorconfig") == LanguageId::Ini);
}

TEST_CASE("GenericSyntaxHighlighter.detectLanguageFromPath", "[tui][highlight]")
{
    CHECK(detectLanguageFromPath("/src/main.cpp") == LanguageId::Cpp);
    CHECK(detectLanguageFromPath("/build/CMakeLists.txt") == LanguageId::CMake);
    CHECK(detectLanguageFromPath("script.py") == LanguageId::Python);
    CHECK(detectLanguageFromPath("run.sh") == LanguageId::Bash);
    CHECK(detectLanguageFromPath("data.json") == LanguageId::Json);
    CHECK(detectLanguageFromPath("config.yml") == LanguageId::Yaml);
    CHECK(detectLanguageFromPath("README.md") == LanguageId::Markdown);
    CHECK(detectLanguageFromPath("noext") == LanguageId::None);

    // Well-known config file names are detected by name (not extension).
    CHECK(detectLanguageFromPath(".clang-format") == LanguageId::Yaml);
    CHECK(detectLanguageFromPath(".clang-tidy") == LanguageId::Yaml);
    CHECK(detectLanguageFromPath(".editorconfig") == LanguageId::Ini);
    // A consumer's own dotfile is not in the table: one was, and a consumer that wants its
    // configuration file highlighted names the language itself.
    CHECK(detectLanguageFromPath(".acme-format") == LanguageId::None);
    // Filename detection works with leading directories too.
    CHECK(detectLanguageFromPath("/home/user/project/.clang-format") == LanguageId::Yaml);
    CHECK(detectLanguageFromPath(R"(C:\proj\.editorconfig)") == LanguageId::Ini);
    // An extension core::tui does not ship falls through to None; a consumer teaches it one
    // through a SyntaxHighlighterRegistry rather than by adding a row here.
    CHECK(detectLanguageFromPath("main.acme") == LanguageId::None);
    // New extensions resolve through the path entry point as well.
    CHECK(detectLanguageFromPath("Build.props") == LanguageId::Xml);
    CHECK(detectLanguageFromPath("deploy.ps1") == LanguageId::PowerShell);
    CHECK(detectLanguageFromPath("setup.bat") == LanguageId::Cmd);
    CHECK(detectLanguageFromPath("settings.ini") == LanguageId::Ini);
}

// =============================================================================
// C++ highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.cpp_keywords", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("if (x) return 0;", LanguageId::Cpp);
    // "if" at positions 0..1 should be Keyword
    CHECK(hasCategory(map, 0, 2, Cat::Keyword));
    // "return" at positions 7..12 should be Keyword
    CHECK(hasCategory(map, 7, 6, Cat::Keyword));
    // "0" at position 14 should be Number
    CHECK(categoryAt(map, 14, Cat::Number));
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.cpp_line_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("int x; // comment", LanguageId::Cpp);
    // "int" at 0..2 is keyword
    CHECK(hasCategory(map, 0, 3, Cat::Keyword));
    // everything from // onwards is comment
    CHECK(hasCategory(map, 7, 10, Cat::Comment));
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.cpp_block_comment_single_line", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("x = /* comment */ y;", LanguageId::Cpp);
    CHECK(hasCategory(map, 4, 13, Cat::Comment));
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.cpp_block_comment_multi_line", "[tui][highlight]")
{
    auto const [map1, state1] = highlightLine("/* start", LanguageId::Cpp);
    CHECK(hasCategory(map1, 0, 8, Cat::Comment));
    CHECK(state1 == HighlightState::BlockComment);

    auto const [map2, state2] = highlightLine("middle", LanguageId::Cpp, state1);
    CHECK(hasCategory(map2, 0, 6, Cat::Comment));
    CHECK(state2 == HighlightState::BlockComment);

    auto const [map3, state3] = highlightLine("end */ code", LanguageId::Cpp, state2);
    CHECK(hasCategory(map3, 0, 5, Cat::Comment));
    CHECK(state3 == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.cpp_string_literal", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(auto s = "hello";)", LanguageId::Cpp);
    // "hello" including quotes at positions 9..15
    CHECK(hasCategory(map, 9, 7, Cat::String));
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.cpp_string_with_escape", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(auto s = "he\"llo";)", LanguageId::Cpp);
    // The entire string including escaped quote
    CHECK(hasCategory(map, 9, 9, Cat::String));
}

TEST_CASE("GenericSyntaxHighlighter.cpp_numbers", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("int x = 42;", LanguageId::Cpp);
    CHECK(hasCategory(map, 8, 2, Cat::Number));
}

TEST_CASE("GenericSyntaxHighlighter.cpp_hex_number", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("auto v = 0xFF;", LanguageId::Cpp);
    CHECK(hasCategory(map, 9, 4, Cat::Number));
}

TEST_CASE("GenericSyntaxHighlighter.cpp_types", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("vector<int> v;", LanguageId::Cpp);
    CHECK(hasCategory(map, 0, 6, Cat::Type));
    CHECK(hasCategory(map, 7, 3, Cat::Keyword)); // int is a keyword
}

TEST_CASE("GenericSyntaxHighlighter.cpp_preprocessor", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("#include <vector>", LanguageId::Cpp);
    CHECK(hasCategory(map, 0, 17, Cat::Preprocessor));
}

TEST_CASE("GenericSyntaxHighlighter.cpp_operators", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("a + b == c", LanguageId::Cpp);
    CHECK(categoryAt(map, 2, Cat::Operator)); // +
    CHECK(categoryAt(map, 6, Cat::Operator)); // first =
    CHECK(categoryAt(map, 7, Cat::Operator)); // second =
}

TEST_CASE("GenericSyntaxHighlighter.cpp_punctuation", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("f(x, y);", LanguageId::Cpp);
    CHECK(categoryAt(map, 1, Cat::Punctuation)); // (
    CHECK(categoryAt(map, 3, Cat::Punctuation)); // ,
    CHECK(categoryAt(map, 6, Cat::Punctuation)); // )
    CHECK(categoryAt(map, 7, Cat::Punctuation)); // ;
}

// =============================================================================
// Python highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.python_keywords", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("def foo(x):", LanguageId::Python);
    CHECK(hasCategory(map, 0, 3, Cat::Keyword)); // def
}

TEST_CASE("GenericSyntaxHighlighter.python_builtins", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("print(len(x))", LanguageId::Python);
    CHECK(hasCategory(map, 0, 5, Cat::Function)); // print
    CHECK(hasCategory(map, 6, 3, Cat::Function)); // len
}

TEST_CASE("GenericSyntaxHighlighter.python_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("x = 1  # note", LanguageId::Python);
    CHECK(hasCategory(map, 7, 7, Cat::Comment));
}

TEST_CASE("GenericSyntaxHighlighter.python_triple_quote", "[tui][highlight]")
{
    auto const [map1, state1] = highlightLine(R"("""start)", LanguageId::Python);
    CHECK(hasCategory(map1, 0, 8, Cat::String));
    CHECK(state1 == HighlightState::TripleQuoteString);

    auto const [map2, state2] = highlightLine("middle", LanguageId::Python, state1);
    CHECK(hasCategory(map2, 0, 6, Cat::String));
    CHECK(state2 == HighlightState::TripleQuoteString);

    auto const [map3, state3] = highlightLine(R"(end""")", LanguageId::Python, state2);
    CHECK(hasCategory(map3, 0, 6, Cat::String));
    CHECK(state3 == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.python_decorator", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("@staticmethod", LanguageId::Python);
    CHECK(hasCategory(map, 0, 13, Cat::Function));
}

// =============================================================================
// Bash highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.bash_keywords", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("if [ -f file ]; then", LanguageId::Bash);
    CHECK(hasCategory(map, 0, 2, Cat::Keyword));  // if
    CHECK(hasCategory(map, 16, 4, Cat::Keyword)); // then
}

TEST_CASE("GenericSyntaxHighlighter.bash_variables", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("echo $HOME ${PATH}", LanguageId::Bash);
    CHECK(hasCategory(map, 5, 5, Cat::Variable));  // $HOME
    CHECK(hasCategory(map, 11, 7, Cat::Variable)); // ${PATH}
}

TEST_CASE("GenericSyntaxHighlighter.bash_builtins", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("export PATH=/usr/bin", LanguageId::Bash);
    CHECK(hasCategory(map, 0, 6, Cat::Function)); // export
}

TEST_CASE("GenericSyntaxHighlighter.bash_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("# this is a comment", LanguageId::Bash);
    CHECK(hasCategory(map, 0, 19, Cat::Comment));
}

// =============================================================================
// CMake highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.cmake_commands", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("add_library(mylib STATIC)", LanguageId::CMake);
    CHECK(hasCategory(map, 0, 11, Cat::Keyword)); // add_library
}

TEST_CASE("GenericSyntaxHighlighter.cmake_variables", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("set(VAR ${CMAKE_SOURCE_DIR})", LanguageId::CMake);
    CHECK(hasCategory(map, 0, 3, Cat::Keyword)); // set
    // ${CMAKE_SOURCE_DIR}
    CHECK(hasCategory(map, 8, 19, Cat::Variable));
}

// =============================================================================
// JSON highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.json_key_value", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(  "name": "value",)", LanguageId::Json);
    // "name" should be Type (it's a key followed by :)
    CHECK(hasCategory(map, 2, 6, Cat::Type));
    // "value" should be String
    CHECK(hasCategory(map, 10, 7, Cat::String));
}

TEST_CASE("GenericSyntaxHighlighter.json_keywords", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("  true, false, null", LanguageId::Json);
    CHECK(hasCategory(map, 2, 4, Cat::Keyword));  // true
    CHECK(hasCategory(map, 8, 5, Cat::Keyword));  // false
    CHECK(hasCategory(map, 15, 4, Cat::Keyword)); // null
}

TEST_CASE("GenericSyntaxHighlighter.json_numbers", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("  42, -3.14", LanguageId::Json);
    CHECK(hasCategory(map, 2, 2, Cat::Number)); // 42
    CHECK(hasCategory(map, 6, 5, Cat::Number)); // -3.14
}

// =============================================================================
// YAML highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.yaml_key_value", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("name: value", LanguageId::Yaml);
    CHECK(hasCategory(map, 0, 4, Cat::Type));    // name
    CHECK(categoryAt(map, 4, Cat::Punctuation)); // :
}

TEST_CASE("GenericSyntaxHighlighter.yaml_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("# comment", LanguageId::Yaml);
    CHECK(hasCategory(map, 0, 9, Cat::Comment));
}

TEST_CASE("GenericSyntaxHighlighter.yaml_keywords", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("enabled: true", LanguageId::Yaml);
    CHECK(hasCategory(map, 0, 7, Cat::Type));    // enabled
    CHECK(hasCategory(map, 9, 4, Cat::Keyword)); // true
}

// =============================================================================
// Git diff highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.gitdiff_lines", "[tui][highlight]")
{
    {
        auto const [map, state] = highlightLine("+added line", LanguageId::GitDiff);
        CHECK(hasCategory(map, 0, 11, Cat::Constructor));
    }
    {
        auto const [map, state] = highlightLine("-removed line", LanguageId::GitDiff);
        CHECK(hasCategory(map, 0, 13, Cat::Variable));
    }
    {
        auto const [map, state] = highlightLine("@@ -1,3 +1,4 @@", LanguageId::GitDiff);
        CHECK(hasCategory(map, 0, 16, Cat::Keyword));
    }
    {
        auto const [map, state] = highlightLine("diff --git a/f b/f", LanguageId::GitDiff);
        CHECK(hasCategory(map, 0, 18, Cat::Comment));
    }
}

// =============================================================================
// Markdown highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.markdown_heading", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("# Heading", LanguageId::Markdown);
    CHECK(hasCategory(map, 0, 9, Cat::Keyword));
}

TEST_CASE("GenericSyntaxHighlighter.markdown_code_span", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("use `code` here", LanguageId::Markdown);
    CHECK(hasCategory(map, 4, 6, Cat::String)); // `code`
}

// =============================================================================
// Assembly highlighting — detection
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.asm_detection_extension", "[tui][highlight]")
{
    CHECK(detectLanguageFromExtension(".asm") == LanguageId::Assembly);
    CHECK(detectLanguageFromExtension(".s") == LanguageId::Assembly);
    CHECK(detectLanguageFromExtension(".S") == LanguageId::Assembly);
    CHECK(detectLanguageFromExtension(".nasm") == LanguageId::Assembly);
}

TEST_CASE("GenericSyntaxHighlighter.asm_detection_fence", "[tui][highlight]")
{
    CHECK(detectLanguageFromFenceTag("asm") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("assembly") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("nasm") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("x86") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("x86asm") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("intel") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("att") == LanguageId::Assembly);
    CHECK(detectLanguageFromFenceTag("gas") == LanguageId::Assembly);
}

// =============================================================================
// Assembly highlighting — Intel syntax
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.asm_intel_instruction", "[tui][highlight]")
{
    // "mov rax, 0x10"
    auto const [map, state] = highlightLine("mov rax, 0x10", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 3, Cat::Keyword));  // mov
    CHECK(hasCategory(map, 4, 3, Cat::Variable)); // rax
    CHECK(hasCategory(map, 9, 4, Cat::Number));   // 0x10
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.asm_intel_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("; this is a comment", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 19, Cat::Comment));
}

TEST_CASE("GenericSyntaxHighlighter.asm_label", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("main:", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 4, Cat::Function)); // main
    CHECK(categoryAt(map, 4, Cat::Punctuation));  // :
}

TEST_CASE("GenericSyntaxHighlighter.asm_nasm_directive", "[tui][highlight]")
{
    // "section .data"
    auto const [map, state] = highlightLine("section .data", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 7, Cat::Preprocessor)); // section
    CHECK(hasCategory(map, 8, 5, Cat::Preprocessor)); // .data
}

TEST_CASE("GenericSyntaxHighlighter.asm_registers", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("push rbp", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 4, Cat::Keyword));  // push
    CHECK(hasCategory(map, 5, 3, Cat::Variable)); // rbp
}

TEST_CASE("GenericSyntaxHighlighter.asm_case_insensitive", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("MOV EAX, EBX", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 3, Cat::Keyword));  // MOV
    CHECK(hasCategory(map, 4, 3, Cat::Variable)); // EAX
    CHECK(hasCategory(map, 9, 3, Cat::Variable)); // EBX
}

TEST_CASE("GenericSyntaxHighlighter.asm_binary_number", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("mov rax, 0b1010", LanguageId::Assembly);
    CHECK(hasCategory(map, 9, 6, Cat::Number)); // 0b1010
}

// =============================================================================
// Assembly highlighting — AT&T syntax
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.asm_att_instruction", "[tui][highlight]")
{
    // "movl $0x10, %eax"
    auto const [map, state] = highlightLine("movl $0x10, %eax", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 4, Cat::Keyword));   // movl
    CHECK(hasCategory(map, 5, 5, Cat::Number));    // $0x10
    CHECK(hasCategory(map, 12, 4, Cat::Variable)); // %eax
}

TEST_CASE("GenericSyntaxHighlighter.asm_att_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("# this is a comment", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 19, Cat::Comment));
}

TEST_CASE("GenericSyntaxHighlighter.asm_gas_directive", "[tui][highlight]")
{
    // ".section .text"
    auto const [map, state] = highlightLine(".section .text", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 8, Cat::Preprocessor)); // .section
    CHECK(hasCategory(map, 9, 5, Cat::Preprocessor)); // .text
}

TEST_CASE("GenericSyntaxHighlighter.asm_string_literal", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(.ascii "hello")", LanguageId::Assembly);
    CHECK(hasCategory(map, 0, 6, Cat::Preprocessor)); // .ascii
    CHECK(hasCategory(map, 7, 7, Cat::String));       // "hello"
}

TEST_CASE("GenericSyntaxHighlighter.asm_token_longer_than_the_lowercase_buffer", "[tui][highlight]")
{
    // The three assembly scanners lowercase an identifier into a 64-character buffer before
    // looking it up. A longer token used to be copied past the end of that buffer, so any
    // rendered ```asm fence could smash the caller's frame; the sanitizer presets are where
    // this case fails without the bound. What it asserts is the visible half: an oversized
    // token matches no keyword table, so it stays ordinary text, and the line around it still
    // highlights.
    auto const longName = std::string(100, 'a');

    SECTION("GAS directive")
    {
        auto const [map, state] = highlightLine("." + longName, LanguageId::Assembly);
        CHECK(hasCategory(map, 0, 101, Cat::Default));
    }

    SECTION("AT&T register")
    {
        auto const [map, state] = highlightLine("movl %" + longName, LanguageId::Assembly);
        CHECK(hasCategory(map, 0, 4, Cat::Keyword)); // movl
        CHECK(hasCategory(map, 5, 101, Cat::Default));
    }

    SECTION("bare identifier")
    {
        auto const [map, state] = highlightLine(longName + " rbp", LanguageId::Assembly);
        CHECK(hasCategory(map, 0, 100, Cat::Default));
        CHECK(hasCategory(map, 101, 3, Cat::Variable)); // rbp
    }
}

TEST_CASE("GenericSyntaxHighlighter.asm_block_comment", "[tui][highlight]")
{
    auto const [map1, state1] = highlightLine("/* start", LanguageId::Assembly);
    CHECK(hasCategory(map1, 0, 8, Cat::Comment));
    CHECK(state1 == HighlightState::BlockComment);

    auto const [map2, state2] = highlightLine("end */", LanguageId::Assembly, state1);
    CHECK(hasCategory(map2, 0, 5, Cat::Comment));
    CHECK(state2 == HighlightState::Normal);
}

// =============================================================================
// PowerShell highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.powershell_variable_and_cmdlet", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("$result = Get-ChildItem", LanguageId::PowerShell);
    CHECK(hasCategory(map, 0, 7, Cat::Variable));   // $result
    CHECK(hasCategory(map, 10, 13, Cat::Function)); // Get-ChildItem (Verb-Noun)
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.powershell_keyword_operator_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("if ($x -eq 1) { } # done", LanguageId::PowerShell);
    CHECK(hasCategory(map, 0, 2, Cat::Keyword));  // if
    CHECK(hasCategory(map, 4, 2, Cat::Variable)); // $x
    CHECK(hasCategory(map, 7, 3, Cat::Operator)); // -eq
    CHECK(categoryAt(map, 11, Cat::Number));      // 1
    CHECK(hasCategory(map, 18, 6, Cat::Comment)); // # done
}

TEST_CASE("GenericSyntaxHighlighter.powershell_string_interpolation", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"("Hello $name")", LanguageId::PowerShell);
    CHECK(hasCategory(map, 0, 7, Cat::String));   // "Hello
    CHECK(hasCategory(map, 7, 5, Cat::Variable)); // $name
    CHECK(categoryAt(map, 12, Cat::String));      // closing quote
}

TEST_CASE("GenericSyntaxHighlighter.powershell_block_comment_multi_line", "[tui][highlight]")
{
    auto const [map1, state1] = highlightLine("<# start", LanguageId::PowerShell);
    CHECK(hasCategory(map1, 0, 8, Cat::Comment));
    CHECK(state1 == HighlightState::PsBlockComment);

    auto const [map2, state2] = highlightLine("middle", LanguageId::PowerShell, state1);
    CHECK(hasCategory(map2, 0, 6, Cat::Comment));
    CHECK(state2 == HighlightState::PsBlockComment);

    auto const [map3, state3] = highlightLine("end #> code", LanguageId::PowerShell, state2);
    CHECK(hasCategory(map3, 0, 6, Cat::Comment));
    CHECK(state3 == HighlightState::Normal);
}

// =============================================================================
// Windows CMD / batch highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.batch_keyword_and_variable", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(set PATH=%PATH%;C:\bin)", LanguageId::Cmd);
    CHECK(hasCategory(map, 0, 3, Cat::Keyword));  // set
    CHECK(hasCategory(map, 9, 6, Cat::Variable)); // %PATH%
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.batch_rem_comment", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("REM this is a comment", LanguageId::Cmd);
    CHECK(hasCategory(map, 0, 21, Cat::Comment));
}

TEST_CASE("GenericSyntaxHighlighter.batch_label", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(":start", LanguageId::Cmd);
    CHECK(hasCategory(map, 0, 6, Cat::Function));
}

TEST_CASE("GenericSyntaxHighlighter.batch_loop_variable", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("echo %%i", LanguageId::Cmd);
    CHECK(hasCategory(map, 0, 4, Cat::Keyword));  // echo
    CHECK(hasCategory(map, 5, 3, Cat::Variable)); // %%i
}

// =============================================================================
// XML highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.xml_tag_attribute_value", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(<tag attr="val">)", LanguageId::Xml);
    CHECK(categoryAt(map, 0, Cat::Punctuation));  // <
    CHECK(hasCategory(map, 1, 3, Cat::Keyword));  // tag
    CHECK(hasCategory(map, 5, 4, Cat::Variable)); // attr
    CHECK(categoryAt(map, 9, Cat::Operator));     // =
    CHECK(hasCategory(map, 10, 5, Cat::String));  // "val"
    CHECK(categoryAt(map, 15, Cat::Punctuation)); // >
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.xml_comment_multi_line", "[tui][highlight]")
{
    auto const [map1, state1] = highlightLine("<!-- start", LanguageId::Xml);
    CHECK(hasCategory(map1, 0, 10, Cat::Comment));
    CHECK(state1 == HighlightState::XmlComment);

    auto const [map2, state2] = highlightLine("still comment", LanguageId::Xml, state1);
    CHECK(hasCategory(map2, 0, 13, Cat::Comment));
    CHECK(state2 == HighlightState::XmlComment);

    auto const [map3, state3] = highlightLine("end -->", LanguageId::Xml, state2);
    CHECK(hasCategory(map3, 0, 7, Cat::Comment));
    CHECK(state3 == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.xml_processing_instruction", "[tui][highlight]")
{
    auto const [map, state] = highlightLine(R"(<?xml version="1.0"?>)", LanguageId::Xml);
    CHECK(hasCategory(map, 0, 21, Cat::Preprocessor));
}

TEST_CASE("GenericSyntaxHighlighter.xml_entity", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("<p>&amp;</p>", LanguageId::Xml);
    CHECK(categoryAt(map, 1, Cat::Keyword));         // p
    CHECK(hasCategory(map, 3, 5, Cat::Constructor)); // &amp;
    CHECK(categoryAt(map, 10, Cat::Keyword));        // p (closing tag)
}

// =============================================================================
// INI / .editorconfig highlighting
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.ini_section", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("[section]", LanguageId::Ini);
    CHECK(hasCategory(map, 0, 9, Cat::Keyword));
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.ini_key_value", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("key = value", LanguageId::Ini);
    CHECK(hasCategory(map, 0, 3, Cat::Variable)); // key
    CHECK(categoryAt(map, 4, Cat::Operator));     // =
    CHECK(hasCategory(map, 6, 5, Cat::String));   // value
}

TEST_CASE("GenericSyntaxHighlighter.ini_comment", "[tui][highlight]")
{
    {
        auto const [map, state] = highlightLine("; comment", LanguageId::Ini);
        CHECK(hasCategory(map, 0, 9, Cat::Comment));
    }
    {
        auto const [map, state] = highlightLine("# comment", LanguageId::Ini);
        CHECK(hasCategory(map, 0, 9, Cat::Comment));
    }
}

TEST_CASE("GenericSyntaxHighlighter.ini_editorconfig_glob", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("indent_size = 4", LanguageId::Ini);
    CHECK(hasCategory(map, 0, 11, Cat::Variable)); // indent_size
    CHECK(categoryAt(map, 12, Cat::Operator));     // =
    CHECK(categoryAt(map, 14, Cat::String));       // 4
}

// =============================================================================
// Empty / None language
// =============================================================================

TEST_CASE("GenericSyntaxHighlighter.empty_line", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("", LanguageId::Cpp);
    CHECK(map.empty());
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("GenericSyntaxHighlighter.none_language", "[tui][highlight]")
{
    auto const [map, state] = highlightLine("some code here", LanguageId::None);
    CHECK(map.empty());
}

// =============================================================================
// The built-in language set is fixed
// =============================================================================

namespace
{
/// @brief What ExtensionLanguageTable ships, row for row.
///
/// A golden copy. The built-in languages are the ones core::tui offers every consumer; an
/// application's own language is registered with a SyntaxHighlighterRegistry instead of being
/// added here. Editing the shipped table without editing this copy fails, so a new row is a
/// decision taken on purpose rather than one a code review has to catch.
constexpr auto ExpectedExtensionTable = std::to_array<LanguageToken>({
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

/// @brief What FenceTagLanguageTable ships, row for row. Same contract as the table above.
constexpr auto ExpectedFenceTagTable = std::to_array<LanguageToken>({
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

/// @brief Compares a shipped language table against its golden copy, row for row.
void checkTableMatchesGolden(std::span<LanguageToken const> shipped, std::span<LanguageToken const> golden)
{
    REQUIRE(shipped.size() == golden.size());
    for (auto const i: std::views::iota(std::size_t { 0 }, golden.size()))
    {
        INFO("row " << i << ", expected token \"" << golden[i].token << '"');
        CHECK(shipped[i].token == golden[i].token);
        CHECK(shipped[i].language == golden[i].language);
    }
}
} // namespace

TEST_CASE("GenericSyntaxHighlighter.extension_table_is_the_shipped_set", "[tui][highlight]")
{
    checkTableMatchesGolden(ExtensionLanguageTable, ExpectedExtensionTable);
}

TEST_CASE("GenericSyntaxHighlighter.fence_tag_table_is_the_shipped_set", "[tui][highlight]")
{
    checkTableMatchesGolden(FenceTagLanguageTable, ExpectedFenceTagTable);
}

TEST_CASE("GenericSyntaxHighlighter.filename_table_is_the_shipped_set", "[tui][highlight]")
{
    // The third table, and the one detectLanguageFromPath() consults first. It is also the table
    // that carried a consumer's `-format` dotfile, and the one that had no golden copy until a
    // review put `{ ".endo-format", Yaml }` back into it and the whole suite stayed green.
    constexpr auto ExpectedFilenameTable = std::to_array<LanguageToken>({
        { .token = "CMakeLists.txt", .language = LanguageId::CMake },
        { .token = "Makefile", .language = LanguageId::Bash },
        { .token = "makefile", .language = LanguageId::Bash },
        { .token = "GNUmakefile", .language = LanguageId::Bash },
        { .token = "Dockerfile", .language = LanguageId::Bash },
        { .token = ".clang-format", .language = LanguageId::Yaml },
        { .token = ".clang-tidy", .language = LanguageId::Yaml },
        { .token = ".editorconfig", .language = LanguageId::Ini },
    });

    checkTableMatchesGolden(FilenameLanguageTable, ExpectedFilenameTable);

    // Every row resolves through the entry point that reads it, so the golden copy pins behaviour
    // and not just data.
    for (auto const& row: FilenameLanguageTable)
    {
        INFO("file name " << row.token);
        CHECK(detectLanguageFromPath(row.token) == row.language);
        CHECK(detectLanguageFromPath(std::string("/home/user/project/") + std::string(row.token))
              == row.language);
    }
}

// =============================================================================
// Registered languages
// =============================================================================

namespace
{
/// @brief One built-in language, with an extension and a fence tag that must select it.
struct BuiltinProbe
{
    LanguageId language;        ///< The language the two tokens must resolve to.
    std::string_view extension; ///< An extension from ExtensionLanguageTable.
    std::string_view fenceTag;  ///< A tag from FenceTagLanguageTable.
};

/// @brief A probe per built-in language, None excepted.
///
/// The static_assert below ties this table's length to the built-in list, so a new built-in
/// language does not compile until somebody says which extension and which fence tag select it.
constexpr auto BuiltinProbes = std::to_array<BuiltinProbe>({
    { .language = LanguageId::Cpp, .extension = ".cpp", .fenceTag = "cpp" },
    { .language = LanguageId::CMake, .extension = ".cmake", .fenceTag = "cmake" },
    { .language = LanguageId::Python, .extension = ".py", .fenceTag = "python" },
    { .language = LanguageId::Bash, .extension = ".sh", .fenceTag = "bash" },
    { .language = LanguageId::Markdown, .extension = ".md", .fenceTag = "markdown" },
    { .language = LanguageId::Json, .extension = ".json", .fenceTag = "json" },
    { .language = LanguageId::Yaml, .extension = ".yml", .fenceTag = "yaml" },
    { .language = LanguageId::GitDiff, .extension = ".diff", .fenceTag = "diff" },
    { .language = LanguageId::Assembly, .extension = ".asm", .fenceTag = "asm" },
    { .language = LanguageId::PowerShell, .extension = ".ps1", .fenceTag = "powershell" },
    { .language = LanguageId::Cmd, .extension = ".bat", .fenceTag = "bat" },
    { .language = LanguageId::Xml, .extension = ".xml", .fenceTag = "xml" },
    { .language = LanguageId::Ini, .extension = ".ini", .fenceTag = "ini" },
});

static_assert(BuiltinProbes.size() + 1 == BuiltinLanguageTable.size(),
              "Every built-in language except None needs a row in BuiltinProbes.");

/// @brief The id a registry issues for its @p index-th registered language.
constexpr auto registeredId(std::size_t index) -> LanguageId
{
    return static_cast<LanguageId>(static_cast<std::uint8_t>(FirstRegisteredLanguageId + index));
}

/// @brief A highlighter that paints every character of a line with one category.
auto uniformHighlighter(Cat category) -> HighlightFunction
{
    return [category](std::string_view line, HighlightState state) {
        return std::pair { HighlightMap(line.size(), category), state };
    };
}
} // namespace

TEST_CASE("GenericSyntaxHighlighter.builtin_language_list_is_the_shipped_set", "[tui][highlight]")
{
    // The names core::tui answers to. A consumer's language is not among them: it is registered.
    constexpr auto ExpectedNames = std::to_array<std::string_view>({
        "none",
        "cpp",
        "cmake",
        "python",
        "bash",
        "markdown",
        "json",
        "yaml",
        "gitdiff",
        "assembly",
        "powershell",
        "cmd",
        "xml",
        "ini",
    });

    REQUIRE(BuiltinLanguageTable.size() == ExpectedNames.size());
    for (auto const i: std::views::iota(std::size_t { 0 }, ExpectedNames.size()))
    {
        INFO("built-in " << i << ", expected name " << ExpectedNames[i]);
        CHECK(BuiltinLanguageTable[i].name == ExpectedNames[i]);
        CHECK(static_cast<std::size_t>(std::to_underlying(BuiltinLanguageTable[i].language)) == i);
    }
}

TEST_CASE("GenericSyntaxHighlighter.no_language_hides_above_Last", "[tui][highlight]")
{
    // Every other case in this file walks `[0, Last)` -- BuiltinLanguageTable, name(), the three
    // golden tables -- so a language appended AFTER `Last` is invisible to all of them. Measured,
    // with such an enumerator actually in the tree:
    //
    //   * appended alone, the build fails on `-Werror,-Wswitch`: highlightBuiltin()'s switch is
    //     the one thing that notices, and it demands a case;
    //   * appended together with the case the compiler just demanded, the build is clean and the
    //     suite green at 3870 assertions in 1033 cases. builtinLanguageTableIsIndexedByLanguageId()
    //     compares the table's size against `Last`, and appending after `Last` moves neither, so
    //     it fires only once the table row is added -- once the mistake is half corrected.
    //
    // What is harmful is a hidden language that *works*: one the switch dispatches to a real
    // highlighter, reachable through a table, or answering to a name. That is what this refuses,
    // and it is the tui half of NetError_test.cpp's "No code hides above Last".
    // `.agent/rules/design-principles.md` names the general version: a check anchored on an
    // enumerator by name fires only when nothing is wrong.
    //
    // What it cannot see: an enumerator above `Last` whose switch case only breaks and which no
    // table names. C++ cannot enumerate enumerators, so nothing can see that one -- but it is also
    // unreachable, unnamed and inert, which is to say it is not yet a language.
    auto const registry = SyntaxHighlighterRegistry {};
    auto const probe = std::string_view { "int x; /* c */" };

    for (auto const value: std::views::iota(static_cast<std::size_t>(std::to_underlying(LanguageId::Last)),
                                            static_cast<std::size_t>(FirstRegisteredLanguageId)))
    {
        auto const language = static_cast<LanguageId>(static_cast<std::uint8_t>(value));
        INFO("LanguageId value " << value);

        // Nothing above the count highlights: all-Default is what an id with no language gives.
        CHECK(highlightLine(probe, language).first == HighlightMap(probe.size(), HighlightCategory::Default));

        // Nothing above the count is named, or reachable from any of the three built-in tables.
        CHECK(registry.name(language).empty());
        CHECK(std::ranges::find(ExtensionLanguageTable, language, &LanguageToken::language)
              == ExtensionLanguageTable.end());
        CHECK(std::ranges::find(FenceTagLanguageTable, language, &LanguageToken::language)
              == FenceTagLanguageTable.end());
        CHECK(std::ranges::find(FilenameLanguageTable, language, &LanguageToken::language)
              == FilenameLanguageTable.end());
    }
}

TEST_CASE("GenericSyntaxHighlighter.every_builtin_detects_from_extension_and_fence_tag", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};
    REQUIRE(registry
                .registerLanguage({ .name = "toy",
                                    .extensions = { ".toy" },
                                    .fenceTags = { "toy" },
                                    .highlight = uniformHighlighter(Cat::Keyword) })
                .has_value());

    for (auto const& probe: BuiltinProbes)
    {
        INFO("built-in " << probe.extension << " / " << probe.fenceTag);
        CHECK(detectLanguageFromExtension(probe.extension) == probe.language);
        CHECK(detectLanguageFromFenceTag(probe.fenceTag) == probe.language);
        // A registry answers for the built-ins too, so registering a language does not cost a
        // consumer the ones core::tui ships.
        CHECK(registry.detectFromExtension(probe.extension) == probe.language);
        CHECK(registry.detectFromFenceTag(probe.fenceTag) == probe.language);
    }
}

TEST_CASE("SyntaxHighlighterRegistry.a_registered_language_is_selected_and_highlights", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};
    auto const toy = registry.registerLanguage({ .name = "toy",
                                                 .extensions = { ".toy" },
                                                 .fenceTags = { "toy" },
                                                 .highlight = uniformHighlighter(Cat::Keyword) });
    REQUIRE(toy.has_value());
    CHECK(isRegisteredLanguage(*toy));
    CHECK(registry.find("toy") == *toy);
    CHECK(registry.name(*toy) == "toy");

    // The extension path.
    CHECK(registry.detectFromExtension(".toy") == *toy);
    CHECK(detectLanguageFromExtension(".toy", &registry) == *toy);
    CHECK(detectLanguageFromPath("/src/main.toy", &registry) == *toy);

    // The Markdown fence path.
    CHECK(registry.detectFromFenceTag("toy") == *toy);
    CHECK(detectLanguageFromFenceTag("toy", &registry) == *toy);

    // The highlight path: the registered function is what runs.
    auto const [map, state] = highlightLine("let x", *toy, HighlightState::Normal, &registry);
    CHECK(map == HighlightMap(5, Cat::Keyword));
    CHECK(state == HighlightState::Normal);
}

TEST_CASE("SyntaxHighlighterRegistry.a_second_registration_does_not_clobber_the_first", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};
    auto const toy = registry.registerLanguage({ .name = "toy",
                                                 .extensions = { ".toy" },
                                                 .fenceTags = { "toy" },
                                                 .highlight = uniformHighlighter(Cat::Keyword) });
    auto const doll = registry.registerLanguage({ .name = "doll",
                                                  .extensions = { ".doll" },
                                                  .fenceTags = { "doll" },
                                                  .highlight = uniformHighlighter(Cat::String) });
    REQUIRE(toy.has_value());
    REQUIRE(doll.has_value());
    CHECK(*toy != *doll);
    CHECK(registry.registeredCount() == 2);

    CHECK(registry.detectFromExtension(".toy") == *toy);
    CHECK(registry.detectFromExtension(".doll") == *doll);
    CHECK(registry.detectFromFenceTag("toy") == *toy);
    CHECK(registry.detectFromFenceTag("doll") == *doll);

    CHECK(highlightLine("ab", *toy, HighlightState::Normal, &registry).first
          == HighlightMap(2, Cat::Keyword));
    CHECK(highlightLine("ab", *doll, HighlightState::Normal, &registry).first
          == HighlightMap(2, Cat::String));
}

TEST_CASE("SyntaxHighlighterRegistry.re_registering_a_name_is_refused", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};
    auto const toy = registry.registerLanguage({ .name = "toy",
                                                 .extensions = { ".toy" },
                                                 .fenceTags = { "toy" },
                                                 .highlight = uniformHighlighter(Cat::Keyword) });
    REQUIRE(toy.has_value());

    // Refused, not replaced: replacing would repoint an id already handed out, which turns every
    // holder of it into a wrong answer that looks right.
    auto const again = registry.registerLanguage({ .name = "toy",
                                                   .extensions = { ".toy2" },
                                                   .fenceTags = { "toy2" },
                                                   .highlight = uniformHighlighter(Cat::Number) });
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().error == LanguageRegistrationError::NameInUse);
    CHECK(again.error().token == "toy");

    // And nothing of the refused definition took effect.
    CHECK(registry.registeredCount() == 1);
    CHECK(registry.detectFromExtension(".toy2") == LanguageId::None);
    CHECK(registry.detectFromFenceTag("toy2") == LanguageId::None);
    CHECK(highlightLine("ab", *toy, HighlightState::Normal, &registry).first
          == HighlightMap(2, Cat::Keyword));
}

TEST_CASE("SyntaxHighlighterRegistry.a_claimed_token_is_refused", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};
    REQUIRE(registry
                .registerLanguage({ .name = "toy",
                                    .extensions = { ".toy" },
                                    .fenceTags = { "toy" },
                                    .highlight = uniformHighlighter(Cat::Keyword) })
                .has_value());

    SECTION("an extension another registered language claimed")
    {
        auto const clash = registry.registerLanguage({ .name = "doll",
                                                       .extensions = { ".toy" },
                                                       .fenceTags = { "doll" },
                                                       .highlight = uniformHighlighter(Cat::String) });
        REQUIRE_FALSE(clash.has_value());
        CHECK(clash.error().error == LanguageRegistrationError::TokenInUse);
        CHECK(clash.error().token == ".toy");
    }

    SECTION("an extension a built-in language claims")
    {
        auto const clash = registry.registerLanguage({ .name = "doll",
                                                       .extensions = { ".cpp" },
                                                       .fenceTags = { "doll" },
                                                       .highlight = uniformHighlighter(Cat::String) });
        REQUIRE_FALSE(clash.has_value());
        CHECK(clash.error().error == LanguageRegistrationError::TokenInUse);
        CHECK(clash.error().token == ".cpp");
    }

    SECTION("a fence tag a built-in language claims")
    {
        auto const clash = registry.registerLanguage({ .name = "doll",
                                                       .extensions = { ".doll" },
                                                       .fenceTags = { "python" },
                                                       .highlight = uniformHighlighter(Cat::String) });
        REQUIRE_FALSE(clash.has_value());
        CHECK(clash.error().error == LanguageRegistrationError::TokenInUse);
        CHECK(clash.error().token == "python");
    }

    SECTION("a name a built-in language answers to")
    {
        auto const clash = registry.registerLanguage({ .name = "cpp",
                                                       .extensions = { ".doll" },
                                                       .fenceTags = { "doll" },
                                                       .highlight = uniformHighlighter(Cat::String) });
        REQUIRE_FALSE(clash.has_value());
        CHECK(clash.error().error == LanguageRegistrationError::NameInUse);
        CHECK(clash.error().token == "cpp");
    }

    // Whatever was refused, the registry is as it was.
    CHECK(registry.registeredCount() == 1);
    CHECK(registry.detectFromExtension(".doll") == LanguageId::None);
    CHECK(registry.detectFromFenceTag("doll") == LanguageId::None);
    CHECK(registry.detectFromExtension(".cpp") == LanguageId::Cpp);
    CHECK(registry.detectFromFenceTag("python") == LanguageId::Python);
}

TEST_CASE("SyntaxHighlighterRegistry.a_token_that_could_never_match_is_refused", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};

    SECTION("an extension without its leading dot")
    {
        // detectLanguageFromPath() looks up from the last dot onwards, so "toy" could never be
        // found. Registering it would succeed and do nothing at all.
        auto const dotless = registry.registerLanguage({ .name = "toy",
                                                         .extensions = { "toy" },
                                                         .fenceTags = { "toy" },
                                                         .highlight = uniformHighlighter(Cat::Keyword) });
        REQUIRE_FALSE(dotless.has_value());
        CHECK(dotless.error().error == LanguageRegistrationError::MalformedToken);
        CHECK(dotless.error().token == "toy");
    }

    SECTION("an extension that is only a dot")
    {
        auto const bare = registry.registerLanguage({ .name = "toy",
                                                      .extensions = { "." },
                                                      .fenceTags = {},
                                                      .highlight = uniformHighlighter(Cat::Keyword) });
        REQUIRE_FALSE(bare.has_value());
        CHECK(bare.error().error == LanguageRegistrationError::MalformedToken);
    }

    SECTION("an empty fence tag, which is what a bare fence yields")
    {
        auto const blank = registry.registerLanguage({ .name = "toy",
                                                       .extensions = { ".toy" },
                                                       .fenceTags = { "" },
                                                       .highlight = uniformHighlighter(Cat::Keyword) });
        REQUIRE_FALSE(blank.has_value());
        CHECK(blank.error().error == LanguageRegistrationError::MalformedToken);
    }

    SECTION("an extension a well-known file name would shadow")
    {
        // `.editorconfig` is matched as a whole file name, before any extension lookup, so this
        // registration would be dead for the one path that spells it exactly.
        auto const shadowed = registry.registerLanguage({ .name = "toy",
                                                          .extensions = { ".editorconfig" },
                                                          .fenceTags = { "toy" },
                                                          .highlight = uniformHighlighter(Cat::Keyword) });
        REQUIRE_FALSE(shadowed.has_value());
        CHECK(shadowed.error().error == LanguageRegistrationError::TokenInUse);
        CHECK(shadowed.error().token == ".editorconfig");
        CHECK(detectLanguageFromPath(".editorconfig", &registry) == LanguageId::Ini);
    }

    CHECK(registry.registeredCount() == 0);
}

TEST_CASE("SyntaxHighlighterRegistry.the_reserved_id_range_runs_out", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};

    // The range is 128 ids wide; the last one issued is 255, and nothing wraps onto a built-in.
    constexpr auto Capacity = std::size_t { 256 } - FirstRegisteredLanguageId;
    for (auto const i: std::views::iota(std::size_t { 0 }, Capacity))
    {
        auto const name = "lang" + std::to_string(i);
        auto const id = registry.registerLanguage({ .name = name,
                                                    .extensions = { "." + name },
                                                    .fenceTags = { name },
                                                    .highlight = uniformHighlighter(Cat::Keyword) });
        INFO("registration " << i);
        REQUIRE(id.has_value());
        CHECK(std::to_underlying(*id) == static_cast<std::uint8_t>(FirstRegisteredLanguageId + i));
    }
    REQUIRE(registry.registeredCount() == Capacity);

    auto const overflow = registry.registerLanguage({ .name = "onetoomany",
                                                      .extensions = { ".onetoomany" },
                                                      .fenceTags = { "onetoomany" },
                                                      .highlight = uniformHighlighter(Cat::Keyword) });
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().error == LanguageRegistrationError::CapacityReached);

    // A full registry still answers, for its own languages and for the built-ins.
    CHECK(registry.registeredCount() == Capacity);
    CHECK(registry.detectFromExtension(".lang0") == registeredId(0));
    CHECK(registry.detectFromExtension(".lang127") == registeredId(Capacity - 1));
    CHECK(registry.detectFromExtension(".cpp") == LanguageId::Cpp);
    CHECK(registry.detectFromExtension(".onetoomany") == LanguageId::None);
}

TEST_CASE("SyntaxHighlighterRegistry.a_nameless_or_mute_definition_is_refused", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};

    auto const unnamed = registry.registerLanguage({ .name = "",
                                                     .extensions = { ".toy" },
                                                     .fenceTags = {},
                                                     .highlight = uniformHighlighter(Cat::Keyword) });
    REQUIRE_FALSE(unnamed.has_value());
    CHECK(unnamed.error().error == LanguageRegistrationError::EmptyName);

    auto const mute = registry.registerLanguage(
        { .name = "toy", .extensions = { ".toy" }, .fenceTags = {}, .highlight = {} });
    REQUIRE_FALSE(mute.has_value());
    CHECK(mute.error().error == LanguageRegistrationError::NoHighlighter);

    CHECK(registry.registeredCount() == 0);
}

TEST_CASE("SyntaxHighlighterRegistry.an_unregistered_token_falls_through_to_none", "[tui][highlight]")
{
    auto registry = SyntaxHighlighterRegistry {};
    REQUIRE(registry
                .registerLanguage({ .name = "toy",
                                    .extensions = { ".toy" },
                                    .fenceTags = { "toy" },
                                    .highlight = uniformHighlighter(Cat::Keyword) })
                .has_value());

    // Exactly as without a registry: an unknown extension or fence tag is None, not the last
    // language registered and not the first row of a table.
    CHECK(registry.detectFromExtension(".nope") == LanguageId::None);
    CHECK(registry.detectFromFenceTag("nope") == LanguageId::None);
    CHECK(registry.detectFromPath("/src/main.nope") == LanguageId::None);
    CHECK(detectLanguageFromExtension(".nope", &registry) == LanguageId::None);
    CHECK(detectLanguageFromFenceTag("nope", &registry) == LanguageId::None);
    CHECK(detectLanguageFromPath("/src/main.nope", &registry) == LanguageId::None);
    CHECK(detectLanguageFromExtension(".nope") == LanguageId::None);
    CHECK(detectLanguageFromFenceTag("nope") == LanguageId::None);
    CHECK(detectLanguageFromPath("/src/main.nope") == LanguageId::None);
}

TEST_CASE("SyntaxHighlighterRegistry.a_registered_id_belongs_to_the_registry_that_issued_it",
          "[tui][highlight]")
{
    auto toys = SyntaxHighlighterRegistry {};
    auto const toy = toys.registerLanguage({ .name = "toy",
                                             .extensions = { ".toy" },
                                             .fenceTags = { "toy" },
                                             .highlight = uniformHighlighter(Cat::Keyword) });
    REQUIRE(toy.has_value());

    // An id this registry did not issue, and cannot resolve, is plain text rather than a guess.
    auto const empty = SyntaxHighlighterRegistry {};
    CHECK(highlightLine("let x", *toy).first == HighlightMap(5, Cat::Default));
    CHECK(highlightLine("let x", *toy, HighlightState::Normal, &empty).first
          == HighlightMap(5, Cat::Default));

    // But that is a consequence of the other registry being empty, not a guarantee. Ids are
    // dense from FirstRegisteredLanguageId in registration order, so a registry that has issued
    // as many ids resolves this one to whatever it registered in that position -- here, a
    // different language, highlighted without complaint. Passing a registered id to a registry
    // that did not issue it is a precondition violation, and this is what it costs.
    auto others = SyntaxHighlighterRegistry {};
    auto const doll = others.registerLanguage({ .name = "doll",
                                                .extensions = { ".doll" },
                                                .fenceTags = { "doll" },
                                                .highlight = uniformHighlighter(Cat::String) });
    REQUIRE(doll.has_value());
    REQUIRE(*doll == *toy); // the first id of either registry, and nothing distinguishes them
    CHECK(highlightLine("let x", *toy, HighlightState::Normal, &others).first
          == HighlightMap(5, Cat::String)); // doll's category, from toy's id

    // The built-in half of LanguageId is portable, because it is not issued by anybody: every
    // registry answers for it identically, and a consumer may pass one anywhere.
    CHECK(toys.detectFromExtension(".cpp") == others.detectFromExtension(".cpp"));
    CHECK(empty.detectFromExtension(".cpp") == LanguageId::Cpp);
    CHECK(toys.highlightLine("int x;", LanguageId::Cpp) == others.highlightLine("int x;", LanguageId::Cpp));
    CHECK(toys.highlightLine("int x;", LanguageId::Cpp) == highlightLine("int x;", LanguageId::Cpp));
}
