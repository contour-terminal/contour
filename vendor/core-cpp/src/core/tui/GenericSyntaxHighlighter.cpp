// SPDX-License-Identifier: Apache-2.0
#include <core/tui/GenericSyntaxHighlighter.hpp>

#include <core/tui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::tui
{

namespace
{
    // -------------------------------------------------------------------------
    // Helper: check if character is part of an identifier
    // -------------------------------------------------------------------------

    constexpr bool isIdentChar(char ch)
    {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
    }

    constexpr bool isDigit(char ch)
    {
        return ch >= '0' && ch <= '9';
    }

    constexpr bool isHexDigit(char ch)
    {
        return isDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    constexpr bool isOperatorChar(char ch)
    {
        return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '=' || ch == '!'
               || ch == '<' || ch == '>' || ch == '&' || ch == '|' || ch == '^' || ch == '~' || ch == '?';
    }

    constexpr bool isPunctuationChar(char ch)
    {
        return ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == '[' || ch == ']' || ch == ','
               || ch == ';' || ch == ':' || ch == '.';
    }

    // -------------------------------------------------------------------------
    // Sorted keyword/type/builtin arrays for binary search
    // -------------------------------------------------------------------------

    // C++ keywords (sorted)
    constexpr auto CppKeywords = std::to_array<std::string_view>({
        "alignas",
        "alignof",
        "and",
        "and_eq",
        "asm",
        "auto",
        "bitand",
        "bitor",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char16_t",
        "char32_t",
        "char8_t",
        "class",
        "co_await",
        "co_return",
        "co_yield",
        "compl",
        "concept",
        "const",
        "const_cast",
        "consteval",
        "constexpr",
        "constinit",
        "continue",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "import",
        "inline",
        "int",
        "long",
        "module",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "not_eq",
        "nullptr",
        "operator",
        "or",
        "or_eq",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "xor",
        "xor_eq",
    });

    // C++ standard library types (sorted)
    constexpr auto CppTypes = std::to_array<std::string_view>({
        "array",      "atomic",   "basic_string", "deque",      "expected",      "function",      "future",
        "int16_t",    "int32_t",  "int64_t",      "int8_t",     "list",          "map",           "monostate",
        "multimap",   "multiset", "optional",     "pair",       "promise",       "ptrdiff_t",     "set",
        "shared_ptr", "size_t",   "span",         "string",     "string_view",   "tuple",         "uint16_t",
        "uint32_t",   "uint64_t", "uint8_t",      "unique_ptr", "unordered_map", "unordered_set", "variant",
        "vector",     "weak_ptr",
    });

    // Python keywords (sorted)
    constexpr auto PythonKeywords = std::to_array<std::string_view>({
        "False", "None",     "True",  "and",    "as",   "assert", "async",  "await",    "break",
        "class", "continue", "def",   "del",    "elif", "else",   "except", "finally",  "for",
        "from",  "global",   "if",    "import", "in",   "is",     "lambda", "nonlocal", "not",
        "or",    "pass",     "raise", "return", "try",  "while",  "with",   "yield",
    });

    // Python builtin functions (sorted)
    constexpr auto PythonBuiltins = std::to_array<std::string_view>({
        "abs",     "all",       "any",        "bin",          "bool",    "bytes",    "callable", "chr",
        "dict",    "dir",       "divmod",     "enumerate",    "eval",    "exec",     "filter",   "float",
        "format",  "frozenset", "getattr",    "globals",      "hasattr", "hash",     "hex",      "id",
        "input",   "int",       "isinstance", "issubclass",   "iter",    "len",      "list",     "locals",
        "map",     "max",       "min",        "next",         "object",  "oct",      "open",     "ord",
        "pow",     "print",     "property",   "range",        "repr",    "reversed", "round",    "set",
        "setattr", "slice",     "sorted",     "staticmethod", "str",     "sum",      "super",    "tuple",
        "type",    "vars",      "zip",
    });

    // Bash keywords (sorted)
    constexpr auto BashKeywords = std::to_array<std::string_view>({
        "case",
        "do",
        "done",
        "elif",
        "else",
        "esac",
        "fi",
        "for",
        "function",
        "if",
        "in",
        "select",
        "then",
        "until",
        "while",
    });

    // Bash builtin commands (sorted)
    constexpr auto BashBuiltins = std::to_array<std::string_view>({
        "alias", "bg",       "bind",   "break",  "cd",      "command", "continue", "declare", "echo",
        "eval",  "exec",     "exit",   "export", "fg",      "getopts", "hash",     "help",    "history",
        "jobs",  "kill",     "let",    "local",  "logout",  "printf",  "pushd",    "popd",    "pwd",
        "read",  "readonly", "return", "set",    "shift",   "shopt",   "source",   "test",    "trap",
        "type",  "typeset",  "ulimit", "umask",  "unalias", "unset",   "wait",
    });

    // CMake commands (sorted)
    constexpr auto CmakeKeywords = std::to_array<std::string_view>({
        "add_compile_definitions",
        "add_compile_options",
        "add_custom_command",
        "add_custom_target",
        "add_definitions",
        "add_dependencies",
        "add_executable",
        "add_library",
        "add_subdirectory",
        "add_test",
        "cmake_minimum_required",
        "cmake_policy",
        "configure_file",
        "else",
        "elseif",
        "enable_language",
        "enable_testing",
        "endforeach",
        "endfunction",
        "endif",
        "endmacro",
        "endwhile",
        "execute_process",
        "export",
        "file",
        "find_file",
        "find_library",
        "find_package",
        "find_path",
        "find_program",
        "foreach",
        "function",
        "get_cmake_property",
        "get_property",
        "if",
        "include",
        "include_directories",
        "install",
        "link_directories",
        "link_libraries",
        "list",
        "macro",
        "mark_as_advanced",
        "math",
        "message",
        "option",
        "project",
        "return",
        "separate_arguments",
        "set",
        "set_property",
        "set_target_properties",
        "string",
        "target_compile_definitions",
        "target_compile_features",
        "target_compile_options",
        "target_include_directories",
        "target_link_directories",
        "target_link_libraries",
        "target_link_options",
        "target_sources",
        "unset",
        "while",
    });

    // CMake variables/properties keywords (sorted)
    constexpr auto CmakeTypes = std::to_array<std::string_view>({
        "AND",          "BOOL",     "CACHE",  "COMMAND",  "DEFINED",      "EXISTS",   "FATAL_ERROR",
        "FALSE",        "FILEPATH", "FORCE",  "IMPORTED", "INTERFACE",    "INTERNAL", "MATCHES",
        "NOT",          "OFF",      "ON",     "OR",       "PARENT_SCOPE", "PATH",     "PRIVATE",
        "PUBLIC",       "REQUIRED", "STATUS", "STREQUAL", "STRING",       "TRUE",     "VERSION_GREATER",
        "VERSION_LESS",
    });

    // JSON keywords (sorted)
    constexpr auto JsonKeywords = std::to_array<std::string_view>({
        "false",
        "null",
        "true",
    });

    // YAML keywords (sorted)
    constexpr auto YamlKeywords = std::to_array<std::string_view>({
        "False",
        "No",
        "Null",
        "Off",
        "On",
        "True",
        "Yes",
        "false",
        "no",
        "null",
        "off",
        "on",
        "true",
        "yes",
        "~",
    });

    // x86 assembly instructions (sorted, lowercase)
    constexpr auto AsmInstructions = std::to_array<std::string_view>({
        "adc",    "add",    "and",    "bsf",     "bsr",    "bt",      "call",     "cbw",     "cdq",
        "cdqe",   "clc",    "cld",    "cli",     "cmova",  "cmovae",  "cmovb",    "cmovbe",  "cmovc",
        "cmove",  "cmovg",  "cmovge", "cmovl",   "cmovle", "cmovna",  "cmovnae",  "cmovnb",  "cmovnbe",
        "cmovnc", "cmovne", "cmovng", "cmovnge", "cmovnl", "cmovnle", "cmovno",   "cmovnp",  "cmovns",
        "cmovnz", "cmovo",  "cmovp",  "cmovs",   "cmovz",  "cmp",     "cmps",     "cmpxchg", "cpuid",
        "cqo",    "cwd",    "cwde",   "dec",     "div",    "enter",   "hlt",      "idiv",    "imul",
        "in",     "inc",    "int",    "iret",    "ja",     "jae",     "jb",       "jbe",     "jc",
        "je",     "jg",     "jge",    "jl",      "jle",    "jmp",     "jna",      "jnae",    "jnb",
        "jnbe",   "jnc",    "jne",    "jng",     "jnge",   "jnl",     "jnle",     "jno",     "jnp",
        "jns",    "jnz",    "jo",     "jp",      "js",     "jz",      "lea",      "leave",   "lock",
        "lods",   "loop",   "loope",  "loopne",  "loopnz", "loopz",   "mov",      "movs",    "movsx",
        "movsxd", "movzx",  "mul",    "neg",     "nop",    "not",     "or",       "out",     "pop",
        "popa",   "popad",  "popf",   "popfd",   "push",   "pusha",   "pushad",   "pushf",   "pushfd",
        "rcl",    "rcr",    "rdtsc",  "rep",     "repe",   "repne",   "repnz",    "repz",    "ret",
        "rol",    "ror",    "sal",    "sar",     "sbb",    "scas",    "seta",     "setae",   "setb",
        "setbe",  "setc",   "sete",   "setg",    "setge",  "setl",    "setle",    "setna",   "setnae",
        "setnb",  "setnbe", "setnc",  "setne",   "setng",  "setnge",  "setnl",    "setnle",  "setno",
        "setnp",  "setns",  "setnz",  "seto",    "setp",   "sets",    "setz",     "shl",     "shr",
        "stc",    "std",    "sti",    "stos",    "sub",    "syscall", "sysenter", "test",    "ud2",
        "xadd",   "xchg",   "xor",
    });

    // x86 registers (sorted, lowercase — matching is case-insensitive)
    constexpr auto AsmRegisters = std::to_array<std::string_view>({
        "ah",    "al",     "ax",    "bh",    "bl",    "bp",    "bpl",   "bx",    "ch",    "cl",     "cr0",
        "cr2",   "cr3",    "cr4",   "cs",    "cx",    "dh",    "di",    "dil",   "dl",    "dr0",    "dr1",
        "dr2",   "dr3",    "dr6",   "dr7",   "ds",    "dx",    "eax",   "ebp",   "ebx",   "ecx",    "edi",
        "edx",   "eflags", "es",    "esi",   "esp",   "fs",    "gs",    "mm0",   "mm1",   "mm2",    "mm3",
        "mm4",   "mm5",    "mm6",   "mm7",   "r10",   "r10b",  "r10d",  "r10w",  "r11",   "r11b",   "r11d",
        "r11w",  "r12",    "r12b",  "r12d",  "r12w",  "r13",   "r13b",  "r13d",  "r13w",  "r14",    "r14b",
        "r14d",  "r14w",   "r15",   "r15b",  "r15d",  "r15w",  "r8",    "r8b",   "r8d",   "r8w",    "r9",
        "r9b",   "r9d",    "r9w",   "rax",   "rbp",   "rbx",   "rcx",   "rdi",   "rdx",   "rflags", "rip",
        "rsi",   "rsp",    "si",    "sil",   "sp",    "spl",   "ss",    "st",    "xmm0",  "xmm1",   "xmm10",
        "xmm11", "xmm12",  "xmm13", "xmm14", "xmm15", "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",   "xmm7",
        "xmm8",  "xmm9",   "ymm0",  "ymm1",  "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15",  "ymm2",
        "ymm3",  "ymm4",   "ymm5",  "ymm6",  "ymm7",  "ymm8",  "ymm9",  "zmm0",  "zmm1",  "zmm10",  "zmm11",
        "zmm12", "zmm13",  "zmm14", "zmm15", "zmm2",  "zmm3",  "zmm4",  "zmm5",  "zmm6",  "zmm7",   "zmm8",
        "zmm9",
    });

    // x86 assembly directives — NASM/MASM style (sorted)
    constexpr auto AsmDirectives = std::to_array<std::string_view>({
        "align", "bits",   "byte",    "common",  "db",      "dd",    "dq",    "dw",    "dword",
        "equ",   "extern", "global",  "incbin",  "include", "org",   "qword", "resb",  "resd",
        "resq",  "resw",   "section", "segment", "times",   "use16", "use32", "use64", "word",
    });

    // AT&T/GAS directives (stored without leading dot, sorted)
    constexpr auto AsmGasDirectives = std::to_array<std::string_view>({
        "align",  "ascii", "asciz",        "bss",    "byte",  "comm",    "data", "endm",    "extern", "file",
        "global", "globl", "intel_syntax", "long",   "macro", "p2align", "quad", "section", "set",    "short",
        "size",   "skip",  "space",        "string", "text",  "type",    "weak", "word",    "zero",
    });

    // PowerShell keywords (sorted, lowercase — matched case-insensitively)
    constexpr auto PowershellKeywords = std::to_array<std::string_view>({
        "begin",        "break",   "catch",   "class",    "continue", "data",   "define",   "do",
        "dynamicparam", "else",    "elseif",  "end",      "enum",     "exit",   "filter",   "finally",
        "for",          "foreach", "from",    "function", "hidden",   "if",     "in",       "inlinescript",
        "parallel",     "param",   "process", "return",   "sequence", "static", "switch",   "throw",
        "trap",         "try",     "until",   "using",    "var",      "while",  "workflow",
    });

    // PowerShell word operators (sorted, lowercase, stored without the leading dash)
    constexpr auto PowershellOperators = std::to_array<std::string_view>({
        "and",      "as",        "band",     "bnot",        "bor",    "bxor",    "ccontains", "ceq",
        "cge",      "cgt",       "cle",      "clike",       "clt",    "cmatch",  "cne",       "cnotcontains",
        "cnotlike", "cnotmatch", "contains", "creplace",    "csplit", "eq",      "f",         "ge",
        "gt",       "in",        "is",       "isnot",       "join",   "le",      "like",      "lt",
        "match",    "ne",        "not",      "notcontains", "notin",  "notlike", "notmatch",  "or",
        "replace",  "shl",       "shr",      "split",       "xor",
    });

    // Windows CMD / batch keywords (sorted, lowercase — matched case-insensitively)
    constexpr auto CmdKeywords = std::to_array<std::string_view>({
        "assoc",    "break",  "call",  "cd",    "chdir",  "cls",      "color",  "copy",   "date",
        "defined",  "del",    "dir",   "echo",  "else",   "endlocal", "equ",    "erase",  "errorlevel",
        "exist",    "exit",   "for",   "ftype", "geq",    "goto",     "gtr",    "if",     "in",
        "leq",      "lss",    "md",    "mkdir", "mklink", "move",     "neq",    "not",    "pause",
        "popd",     "prompt", "pushd", "rd",    "rem",    "ren",      "rename", "rmdir",  "set",
        "setlocal", "shift",  "start", "time",  "title",  "type",     "ver",    "verify", "vol",
    });

    /// @brief Converts an identifier to lowercase into a caller-supplied buffer.
    ///
    /// An identifier longer than @p buf is returned unchanged rather than written past the end
    /// of it. Every table this feeds holds short lowercase keywords, so an oversized token
    /// matches nothing either way, and holding the bound here means a call site cannot forget
    /// it — three of the seven did, and a 64-character token in a ```asm fence then overran a
    /// stack buffer.
    ///
    /// @param src The source identifier.
    /// @param buf Destination buffer.
    /// @return A view of the lowercase copy in @p buf, or @p src when it does not fit.
    [[nodiscard]] auto toLowerInto(std::string_view src, std::span<char> buf) -> std::string_view
    {
        if (src.size() > buf.size())
            return src;
        for (auto const i: std::views::iota(std::size_t { 0 }, src.size()))
            buf[i] = static_cast<char>(src[i] >= 'A' && src[i] <= 'Z' ? src[i] + ('a' - 'A') : src[i]);
        return { buf.data(), src.size() };
    }

    /// @brief Binary search in a sorted array of string_views.
    template <std::size_t N>
    constexpr bool isInSortedArray(std::array<std::string_view, N> const& arr, std::string_view word)
    {
        return std::ranges::binary_search(arr, word);
    }

    // -------------------------------------------------------------------------
    // Fill helpers
    // -------------------------------------------------------------------------

    void fillRange(HighlightMap& map, std::size_t start, std::size_t count, HighlightCategory cat)
    {
        auto const end = std::min(start + count, map.size());
        for (auto const i: std::views::iota(start, std::max(start, end)))
            map[i] = cat;
    }

    // -------------------------------------------------------------------------
    // Per-character number scanning
    // -------------------------------------------------------------------------

    /// @brief Scans a numeric literal starting at pos, returns length consumed.
    std::size_t scanNumber(std::string_view line, std::size_t pos)
    {
        auto i = pos;
        auto const len = line.size();

        // Hex: 0x...
        if (i + 1 < len && line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X'))
        {
            i += 2;
            while (i < len && isHexDigit(line[i]))
                ++i;
            // Suffix
            while (i < len && (line[i] == 'u' || line[i] == 'U' || line[i] == 'l' || line[i] == 'L'))
                ++i;
            return i - pos;
        }

        // Binary: 0b...
        if (i + 1 < len && line[i] == '0' && (line[i + 1] == 'b' || line[i + 1] == 'B'))
        {
            i += 2;
            while (i < len && (line[i] == '0' || line[i] == '1' || line[i] == '\''))
                ++i;
            return i - pos;
        }

        // Octal or decimal
        while (i < len && (isDigit(line[i]) || line[i] == '\''))
            ++i;

        // Decimal point
        if (i < len && line[i] == '.')
        {
            ++i;
            while (i < len && (isDigit(line[i]) || line[i] == '\''))
                ++i;
        }

        // Exponent
        if (i < len && (line[i] == 'e' || line[i] == 'E'))
        {
            ++i;
            if (i < len && (line[i] == '+' || line[i] == '-'))
                ++i;
            while (i < len && isDigit(line[i]))
                ++i;
        }

        // Suffix (f, F, l, L, u, U, ll, LL, etc.)
        while (i < len
               && (line[i] == 'f' || line[i] == 'F' || line[i] == 'l' || line[i] == 'L' || line[i] == 'u'
                   || line[i] == 'U'))
            ++i;

        return i - pos;
    }

    // -------------------------------------------------------------------------
    // C++ highlighter
    // -------------------------------------------------------------------------

    auto highlightCpp(std::string_view line, HighlightState state) -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Continue block comment from previous line
        if (state == HighlightState::BlockComment)
        {
            while (pos < len)
            {
                if (pos + 1 < len && line[pos] == '*' && line[pos + 1] == '/')
                {
                    map[pos] = HighlightCategory::Comment;
                    map[pos + 1] = HighlightCategory::Comment;
                    pos += 2;
                    state = HighlightState::Normal;
                    break;
                }
                map[pos] = HighlightCategory::Comment;
                ++pos;
            }
            if (state == HighlightState::BlockComment)
            {
                return { std::move(map), state };
            }
        }

        // Continue raw string from previous line
        if (state == HighlightState::RawString)
        {
            while (pos < len)
            {
                // Look for )" to end raw string (simplified — doesn't track delimiter)
                if (pos + 1 < len && line[pos] == ')' && line[pos + 1] == '"')
                {
                    map[pos] = HighlightCategory::String;
                    map[pos + 1] = HighlightCategory::String;
                    pos += 2;
                    state = HighlightState::Normal;
                    break;
                }
                map[pos] = HighlightCategory::String;
                ++pos;
            }
            if (state == HighlightState::RawString)
            {
                return { std::move(map), state };
            }
        }

        while (pos < len)
        {
            auto const ch = line[pos];

            // Line comment
            if (pos + 1 < len && ch == '/' && line[pos + 1] == '/')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // Block comment
            if (pos + 1 < len && ch == '/' && line[pos + 1] == '*')
            {
                map[pos] = HighlightCategory::Comment;
                map[pos + 1] = HighlightCategory::Comment;
                pos += 2;
                state = HighlightState::BlockComment;
                while (pos < len)
                {
                    if (pos + 1 < len && line[pos] == '*' && line[pos + 1] == '/')
                    {
                        map[pos] = HighlightCategory::Comment;
                        map[pos + 1] = HighlightCategory::Comment;
                        pos += 2;
                        state = HighlightState::Normal;
                        break;
                    }
                    map[pos] = HighlightCategory::Comment;
                    ++pos;
                }
                continue;
            }

            // Raw string R"(...)"
            if (ch == 'R' && pos + 1 < len && line[pos + 1] == '"')
            {
                map[pos] = HighlightCategory::String;
                map[pos + 1] = HighlightCategory::String;
                pos += 2;
                // Skip delimiter until (
                while (pos < len && line[pos] != '(')
                {
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                if (pos < len)
                {
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                state = HighlightState::RawString;
                // Search for )" on same line
                while (pos < len)
                {
                    if (pos + 1 < len && line[pos] == ')' && line[pos + 1] == '"')
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        state = HighlightState::Normal;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // String literals
            if (ch == '"' || ch == '\'')
            {
                auto const quote = ch;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == quote)
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Preprocessor directives
            if (ch == '#')
            {
                // Check if this is a preprocessor line (# at start after optional whitespace)
                auto isPreproc = true;
                for (auto const j: std::views::iota(std::size_t { 0 }, pos))
                {
                    if (line[j] != ' ' && line[j] != '\t')
                    {
                        isPreproc = false;
                        break;
                    }
                }
                if (isPreproc)
                {
                    fillRange(map, pos, len - pos, HighlightCategory::Preprocessor);
                    return { std::move(map), HighlightState::Normal };
                }
            }

            // Numbers
            if (isDigit(ch) || (ch == '.' && pos + 1 < len && isDigit(line[pos + 1])))
            {
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Identifiers / keywords / types
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const word = line.substr(start, pos - start);

                if (isInSortedArray(CppKeywords, word))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                else if (isInSortedArray(CppTypes, word))
                    fillRange(map, start, pos - start, HighlightCategory::Type);
                else if (word.starts_with("std"))
                    fillRange(map, start, pos - start, HighlightCategory::Type);
                continue;
            }

            // Operators
            if (isOperatorChar(ch))
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }

            // Punctuation
            if (isPunctuationChar(ch))
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            // Default (whitespace, etc.)
            ++pos;
        }

        return { std::move(map), state };
    }

    // -------------------------------------------------------------------------
    // Python highlighter
    // -------------------------------------------------------------------------

    auto highlightPython(std::string_view line, HighlightState state)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Continue triple-quoted string from previous line
        if (state == HighlightState::TripleQuoteString)
        {
            while (pos < len)
            {
                if (pos + 2 < len
                    && ((line[pos] == '"' && line[pos + 1] == '"' && line[pos + 2] == '"')
                        || (line[pos] == '\'' && line[pos + 1] == '\'' && line[pos + 2] == '\'')))
                {
                    map[pos] = HighlightCategory::String;
                    map[pos + 1] = HighlightCategory::String;
                    map[pos + 2] = HighlightCategory::String;
                    pos += 3;
                    state = HighlightState::Normal;
                    break;
                }
                map[pos] = HighlightCategory::String;
                ++pos;
            }
            if (state == HighlightState::TripleQuoteString)
                return { std::move(map), state };
        }

        while (pos < len)
        {
            auto const ch = line[pos];

            // Line comment
            if (ch == '#')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), state };
            }

            // Decorator
            if (ch == '@' && (pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == '\t'))
            {
                auto const start = pos;
                ++pos;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                fillRange(map, start, pos - start, HighlightCategory::Function);
                continue;
            }

            // Triple-quoted strings
            if (pos + 2 < len
                && ((ch == '"' && line[pos + 1] == '"' && line[pos + 2] == '"')
                    || (ch == '\'' && line[pos + 1] == '\'' && line[pos + 2] == '\'')))
            {
                auto const quote3 = line.substr(pos, 3);
                map[pos] = HighlightCategory::String;
                map[pos + 1] = HighlightCategory::String;
                map[pos + 2] = HighlightCategory::String;
                pos += 3;
                state = HighlightState::TripleQuoteString;
                while (pos < len)
                {
                    if (pos + 2 < len && line.substr(pos, 3) == quote3)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        map[pos + 2] = HighlightCategory::String;
                        pos += 3;
                        state = HighlightState::Normal;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Regular strings
            if (ch == '"' || ch == '\'')
            {
                auto const quote = ch;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == quote)
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Numbers
            if (isDigit(ch) || (ch == '.' && pos + 1 < len && isDigit(line[pos + 1])))
            {
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Identifiers / keywords / builtins
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const word = line.substr(start, pos - start);

                if (isInSortedArray(PythonKeywords, word))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                else if (isInSortedArray(PythonBuiltins, word))
                    fillRange(map, start, pos - start, HighlightCategory::Function);
                continue;
            }

            // Operators
            if (isOperatorChar(ch))
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }

            // Punctuation
            if (isPunctuationChar(ch))
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), state };
    }

    // -------------------------------------------------------------------------
    // Bash highlighter
    // -------------------------------------------------------------------------

    auto highlightBash(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        while (pos < len)
        {
            auto const ch = line[pos];

            // Line comment
            if (ch == '#')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // Variable: $VAR or ${VAR}
            if (ch == '$')
            {
                auto const start = pos;
                ++pos;
                if (pos < len && line[pos] == '{')
                {
                    ++pos;
                    while (pos < len && line[pos] != '}')
                        ++pos;
                    if (pos < len)
                        ++pos; // skip '}'
                }
                else if (pos < len && line[pos] == '(')
                {
                    // $(...) command substitution — just color the $
                    fillRange(map, start, 1, HighlightCategory::Variable);
                    continue;
                }
                else
                {
                    while (pos < len && isIdentChar(line[pos]))
                        ++pos;
                }
                fillRange(map, start, pos - start, HighlightCategory::Variable);
                continue;
            }

            // Strings
            if (ch == '"' || ch == '\'')
            {
                auto const quote = ch;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (quote == '"' && line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == quote)
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    // Highlight variables inside double quotes
                    if (quote == '"' && line[pos] == '$')
                    {
                        auto const varStart = pos;
                        ++pos;
                        if (pos < len && line[pos] == '{')
                        {
                            ++pos;
                            while (pos < len && line[pos] != '}')
                                ++pos;
                            if (pos < len)
                                ++pos;
                        }
                        else
                        {
                            while (pos < len && isIdentChar(line[pos]))
                                ++pos;
                        }
                        fillRange(map, varStart, pos - varStart, HighlightCategory::Variable);
                        continue;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Numbers
            if (isDigit(ch))
            {
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Identifiers / keywords / builtins
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const word = line.substr(start, pos - start);

                if (isInSortedArray(BashKeywords, word))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                else if (isInSortedArray(BashBuiltins, word))
                    fillRange(map, start, pos - start, HighlightCategory::Function);
                continue;
            }

            // Operators
            if (isOperatorChar(ch) || ch == '@')
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }

            // Punctuation
            if (isPunctuationChar(ch))
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // CMake highlighter
    // -------------------------------------------------------------------------

    auto highlightCMake(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        while (pos < len)
        {
            auto const ch = line[pos];

            // Line comment
            if (ch == '#')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // Variable reference ${...}
            if (ch == '$' && pos + 1 < len && line[pos + 1] == '{')
            {
                auto const start = pos;
                pos += 2;
                while (pos < len && line[pos] != '}')
                    ++pos;
                if (pos < len)
                    ++pos;
                fillRange(map, start, pos - start, HighlightCategory::Variable);
                continue;
            }

            // Strings
            if (ch == '"')
            {
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == '$' && pos + 1 < len && line[pos + 1] == '{')
                    {
                        auto const varStart = pos;
                        pos += 2;
                        while (pos < len && line[pos] != '}')
                            ++pos;
                        if (pos < len)
                            ++pos;
                        fillRange(map, varStart, pos - varStart, HighlightCategory::Variable);
                        continue;
                    }
                    if (line[pos] == '"')
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Identifiers / commands / types
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && (isIdentChar(line[pos]) || line[pos] == '-'))
                    ++pos;
                auto const word = line.substr(start, pos - start);

                if (isInSortedArray(CmakeKeywords, word))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                else if (isInSortedArray(CmakeTypes, word))
                    fillRange(map, start, pos - start, HighlightCategory::Type);
                continue;
            }

            // Punctuation
            if (isPunctuationChar(ch))
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // Markdown highlighter
    // -------------------------------------------------------------------------

    auto highlightMarkdown(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);

        if (len == 0)
            return { std::move(map), HighlightState::Normal };

        // Headings: # ...
        if (line[0] == '#')
        {
            fillRange(map, 0, len, HighlightCategory::Keyword);
            return { std::move(map), HighlightState::Normal };
        }

        auto pos = std::size_t { 0 };
        while (pos < len)
        {
            auto const ch = line[pos];

            // Bold/italic markers
            if (ch == '*' || ch == '_')
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }

            // Backtick code spans
            if (ch == '`')
            {
                auto const start = pos;
                ++pos;
                while (pos < len && line[pos] != '`')
                    ++pos;
                if (pos < len)
                    ++pos;
                fillRange(map, start, pos - start, HighlightCategory::String);
                continue;
            }

            // Links: [text](url)
            if (ch == '[')
            {
                auto const start = pos;
                auto const closeBracket = line.find(']', pos + 1);
                if (closeBracket != std::string_view::npos && closeBracket + 1 < len
                    && line[closeBracket + 1] == '(')
                {
                    auto const closeParen = line.find(')', closeBracket + 2);
                    if (closeParen != std::string_view::npos)
                    {
                        fillRange(map, start, closeParen + 1 - start, HighlightCategory::Function);
                        pos = closeParen + 1;
                        continue;
                    }
                }
            }

            ++pos;
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // JSON highlighter
    // -------------------------------------------------------------------------

    auto highlightJson(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        while (pos < len)
        {
            auto const ch = line[pos];

            // Strings — check if this is a key (followed by : after closing quote)
            if (ch == '"')
            {
                auto const start = pos;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == '"')
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }

                // Check if followed by colon (making this a key)
                auto afterQuote = pos;
                while (afterQuote < len && (line[afterQuote] == ' ' || line[afterQuote] == '\t'))
                    ++afterQuote;
                if (afterQuote < len && line[afterQuote] == ':')
                    fillRange(map, start, pos - start, HighlightCategory::Type);
                continue;
            }

            // Numbers
            if (isDigit(ch) || (ch == '-' && pos + 1 < len && isDigit(line[pos + 1])))
            {
                if (ch == '-')
                {
                    map[pos] = HighlightCategory::Number;
                    ++pos;
                }
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Keywords: true, false, null
            if ((ch >= 'a' && ch <= 'z'))
            {
                auto const start = pos;
                while (pos < len && (line[pos] >= 'a' && line[pos] <= 'z'))
                    ++pos;
                auto const word = line.substr(start, pos - start);
                if (isInSortedArray(JsonKeywords, word))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                continue;
            }

            // Punctuation
            if (ch == '{' || ch == '}' || ch == '[' || ch == ']' || ch == ',' || ch == ':')
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // YAML highlighter
    // -------------------------------------------------------------------------

    auto highlightYaml(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Skip leading whitespace
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;

        if (pos >= len)
            return { std::move(map), HighlightState::Normal };

        // Comment
        if (line[pos] == '#')
        {
            fillRange(map, pos, len - pos, HighlightCategory::Comment);
            return { std::move(map), HighlightState::Normal };
        }

        // Document markers
        if (line == "---" || line == "...")
        {
            fillRange(map, 0, len, HighlightCategory::Keyword);
            return { std::move(map), HighlightState::Normal };
        }

        // List marker
        if (line[pos] == '-' && pos + 1 < len && line[pos + 1] == ' ')
        {
            map[pos] = HighlightCategory::Punctuation;
            pos += 2;
        }

        // Key: value detection
        while (pos < len)
        {
            auto const ch = line[pos];

            // Check for key (identifier followed by colon)
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-')
            {
                auto const start = pos;
                while (pos < len && line[pos] != ':' && line[pos] != ' ' && line[pos] != '\t'
                       && line[pos] != '#')
                    ++pos;
                if (pos < len && line[pos] == ':')
                {
                    fillRange(map, start, pos - start, HighlightCategory::Type);
                    map[pos] = HighlightCategory::Punctuation;
                    ++pos;
                    // Skip space after colon
                    while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
                        ++pos;

                    // Highlight the value
                    if (pos < len)
                    {
                        // Comment in value
                        auto const commentPos = line.find(" #", pos);
                        auto const valueEnd = (commentPos != std::string_view::npos) ? commentPos : len;

                        auto const valueStr = line.substr(pos, valueEnd - pos);
                        // Trim trailing whitespace for keyword check
                        auto trimmed = valueStr;
                        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
                            trimmed.remove_suffix(1);

                        if (isInSortedArray(YamlKeywords, trimmed))
                            fillRange(map, pos, valueEnd - pos, HighlightCategory::Keyword);
                        else if (!trimmed.empty()
                                 && (isDigit(trimmed[0])
                                     || (trimmed[0] == '-' && trimmed.size() > 1 && isDigit(trimmed[1]))))
                            fillRange(map, pos, valueEnd - pos, HighlightCategory::Number);
                        else if (!trimmed.empty() && (trimmed[0] == '"' || trimmed[0] == '\''))
                            fillRange(map, pos, valueEnd - pos, HighlightCategory::String);

                        pos = valueEnd;
                        // Highlight trailing comment
                        if (commentPos != std::string_view::npos)
                        {
                            fillRange(map, commentPos, len - commentPos, HighlightCategory::Comment);
                            pos = len;
                        }
                    }
                    continue;
                }
                // Not a key, check if it's a keyword value
                auto const word = line.substr(start, pos - start);
                if (isInSortedArray(YamlKeywords, word))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                continue;
            }

            // Strings
            if (ch == '"' || ch == '\'')
            {
                auto const quote = ch;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == quote)
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Comment
            if (ch == '#')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // Numbers
            if (isDigit(ch))
            {
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            ++pos;
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // Git diff highlighter
    // -------------------------------------------------------------------------

    auto highlightGitDiff(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);

        if (len == 0)
            return { std::move(map), HighlightState::Normal };

        // Diff headers: diff, index, ---, +++
        if (line.starts_with("diff ") || line.starts_with("index ") || line.starts_with("--- ")
            || line.starts_with("+++ "))
        {
            fillRange(map, 0, len, HighlightCategory::Comment);
            return { std::move(map), HighlightState::Normal };
        }

        // Hunk headers: @@ ... @@
        if (line.starts_with("@@"))
        {
            fillRange(map, 0, len, HighlightCategory::Keyword);
            return { std::move(map), HighlightState::Normal };
        }

        // Addition lines: +
        if (line[0] == '+')
        {
            fillRange(map, 0, len, HighlightCategory::Constructor);
            return { std::move(map), HighlightState::Normal };
        }

        // Deletion lines: -
        if (line[0] == '-')
        {
            fillRange(map, 0, len, HighlightCategory::Variable);
            return { std::move(map), HighlightState::Normal };
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // x86 Assembly highlighter (Intel + AT&T syntax)
    // -------------------------------------------------------------------------

    auto highlightAssembly(std::string_view line, HighlightState state)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Continue block comment from previous line
        if (state == HighlightState::BlockComment)
        {
            while (pos < len)
            {
                if (pos + 1 < len && line[pos] == '*' && line[pos + 1] == '/')
                {
                    map[pos] = HighlightCategory::Comment;
                    map[pos + 1] = HighlightCategory::Comment;
                    pos += 2;
                    state = HighlightState::Normal;
                    break;
                }
                map[pos] = HighlightCategory::Comment;
                ++pos;
            }
            if (state == HighlightState::BlockComment)
                return { std::move(map), state };
        }

        while (pos < len)
        {
            auto const ch = line[pos];

            // Intel-style line comment: ;
            if (ch == ';')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // AT&T/GAS line comment: #
            if (ch == '#')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // C-style block comment: /* ... */
            if (pos + 1 < len && ch == '/' && line[pos + 1] == '*')
            {
                map[pos] = HighlightCategory::Comment;
                map[pos + 1] = HighlightCategory::Comment;
                pos += 2;
                state = HighlightState::BlockComment;
                while (pos < len)
                {
                    if (pos + 1 < len && line[pos] == '*' && line[pos + 1] == '/')
                    {
                        map[pos] = HighlightCategory::Comment;
                        map[pos + 1] = HighlightCategory::Comment;
                        pos += 2;
                        state = HighlightState::Normal;
                        break;
                    }
                    map[pos] = HighlightCategory::Comment;
                    ++pos;
                }
                continue;
            }

            // String literals
            if (ch == '"' || ch == '\'')
            {
                auto const quote = ch;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (line[pos] == '\\' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == quote)
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // GAS directives: .identifier
            if (ch == '.' && pos + 1 < len
                && ((line[pos + 1] >= 'a' && line[pos + 1] <= 'z')
                    || (line[pos + 1] >= 'A' && line[pos + 1] <= 'Z') || line[pos + 1] == '_'))
            {
                auto const start = pos;
                ++pos; // skip '.'
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const directive = line.substr(start + 1, pos - start - 1);
                std::array<char, 64> lowerBuf {};
                auto const lowerDirective = toLowerInto(directive, lowerBuf);
                if (isInSortedArray(AsmGasDirectives, lowerDirective))
                    fillRange(map, start, pos - start, HighlightCategory::Preprocessor);
                continue;
            }

            // AT&T register syntax: %register
            if (ch == '%' && pos + 1 < len
                && ((line[pos + 1] >= 'a' && line[pos + 1] <= 'z')
                    || (line[pos + 1] >= 'A' && line[pos + 1] <= 'Z')))
            {
                auto const start = pos;
                ++pos; // skip '%'
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const regName = line.substr(start + 1, pos - start - 1);
                std::array<char, 64> lowerBuf {};
                auto const lowerReg = toLowerInto(regName, lowerBuf);
                if (isInSortedArray(AsmRegisters, lowerReg))
                    fillRange(map, start, pos - start, HighlightCategory::Variable);
                continue;
            }

            // AT&T immediate: $number
            if (ch == '$' && pos + 1 < len
                && (isDigit(line[pos + 1])
                    || (pos + 2 < len && line[pos + 1] == '0'
                        && (line[pos + 2] == 'x' || line[pos + 2] == 'X'))))
            {
                map[pos] = HighlightCategory::Number;
                ++pos;
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Numbers: hex (0x), binary (0b), decimal
            if (isDigit(ch))
            {
                auto const numStart = pos;
                auto const numLen = scanNumber(line, pos);
                pos += numLen;
                // Check for Intel hex suffix (e.g., 0FFh)
                if (pos < len && (line[pos] == 'h' || line[pos] == 'H'))
                    ++pos;
                fillRange(map, numStart, pos - numStart, HighlightCategory::Number);
                continue;
            }

            // Identifiers: instructions, registers, directives, labels
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const word = line.substr(start, pos - start);

                // Label: identifier followed by ':'
                if (pos < len && line[pos] == ':')
                {
                    fillRange(map, start, pos - start, HighlightCategory::Function);
                    map[pos] = HighlightCategory::Punctuation;
                    ++pos;
                    continue;
                }

                // Case-insensitive matching
                std::array<char, 64> lowerBuf {};
                auto const lower = toLowerInto(word, lowerBuf);

                // Check instructions, including AT&T suffix stripping (b/w/l/q)
                auto isInstruction = isInSortedArray(AsmInstructions, lower);
                if (!isInstruction && lower.size() > 1)
                {
                    auto const lastCh = lower.back();
                    if (lastCh == 'b' || lastCh == 'w' || lastCh == 'l' || lastCh == 'q')
                        isInstruction = isInSortedArray(AsmInstructions, lower.substr(0, lower.size() - 1));
                }

                if (isInstruction)
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                else if (isInSortedArray(AsmRegisters, lower))
                    fillRange(map, start, pos - start, HighlightCategory::Variable);
                else if (isInSortedArray(AsmDirectives, lower))
                    fillRange(map, start, pos - start, HighlightCategory::Preprocessor);
                continue;
            }

            // Operators / punctuation
            if (ch == '+' || ch == '-' || ch == '*')
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }
            if (ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ',')
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), state };
    }

    // -------------------------------------------------------------------------
    // PowerShell highlighter
    // -------------------------------------------------------------------------

    /// @brief Highlights a line of PowerShell, tracking <# … #> block comments via state.
    auto highlightPowerShell(std::string_view line, HighlightState state)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Continue a <# … #> block comment carried over from the previous line.
        if (state == HighlightState::PsBlockComment)
        {
            while (pos < len)
            {
                if (pos + 1 < len && line[pos] == '#' && line[pos + 1] == '>')
                {
                    map[pos] = HighlightCategory::Comment;
                    map[pos + 1] = HighlightCategory::Comment;
                    pos += 2;
                    state = HighlightState::Normal;
                    break;
                }
                map[pos] = HighlightCategory::Comment;
                ++pos;
            }
            if (state == HighlightState::PsBlockComment)
                return { std::move(map), state };
        }

        while (pos < len)
        {
            auto const ch = line[pos];

            // Block comment: <# … #>
            if (ch == '<' && pos + 1 < len && line[pos + 1] == '#')
            {
                auto const start = pos;
                pos += 2;
                state = HighlightState::PsBlockComment;
                while (pos < len)
                {
                    if (pos + 1 < len && line[pos] == '#' && line[pos + 1] == '>')
                    {
                        pos += 2;
                        state = HighlightState::Normal;
                        break;
                    }
                    ++pos;
                }
                fillRange(map, start, pos - start, HighlightCategory::Comment);
                continue;
            }

            // Line comment: #
            if (ch == '#')
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }

            // Variable: $var, ${var}, $env:NAME, $script:var, …
            if (ch == '$')
            {
                auto const start = pos;
                ++pos;
                if (pos < len && line[pos] == '{')
                {
                    ++pos;
                    while (pos < len && line[pos] != '}')
                        ++pos;
                    if (pos < len)
                        ++pos; // skip '}'
                }
                else
                {
                    // identifier chars plus ':' for scope qualifiers (env:, script:, global:)
                    while (pos < len && (isIdentChar(line[pos]) || line[pos] == ':'))
                        ++pos;
                }
                fillRange(map, start, pos - start, HighlightCategory::Variable);
                continue;
            }

            // Word operator: -eq, -match, -not, … (a dash immediately followed by letters)
            if (ch == '-' && pos + 1 < len
                && ((line[pos + 1] >= 'a' && line[pos + 1] <= 'z')
                    || (line[pos + 1] >= 'A' && line[pos + 1] <= 'Z')))
            {
                auto const start = pos;
                auto const wordStart = pos + 1;
                pos = wordStart;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const word = line.substr(wordStart, pos - wordStart);
                std::array<char, 64> buf {};
                auto const lower = toLowerInto(word, buf);
                if (isInSortedArray(PowershellOperators, lower))
                    fillRange(map, start, pos - start, HighlightCategory::Operator);
                // Otherwise a parameter name like -Path — left as default text.
                continue;
            }

            // Strings (single- and double-quoted; backtick escapes inside double quotes)
            if (ch == '"' || ch == '\'')
            {
                auto const quote = ch;
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    if (quote == '"' && line[pos] == '`' && pos + 1 < len)
                    {
                        map[pos] = HighlightCategory::String;
                        map[pos + 1] = HighlightCategory::String;
                        pos += 2;
                        continue;
                    }
                    if (line[pos] == quote)
                    {
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        break;
                    }
                    if (quote == '"' && line[pos] == '$')
                    {
                        auto const varStart = pos;
                        ++pos;
                        if (pos < len && line[pos] == '{')
                        {
                            ++pos;
                            while (pos < len && line[pos] != '}')
                                ++pos;
                            if (pos < len)
                                ++pos;
                        }
                        else
                        {
                            while (pos < len && (isIdentChar(line[pos]) || line[pos] == ':'))
                                ++pos;
                        }
                        fillRange(map, varStart, pos - varStart, HighlightCategory::Variable);
                        continue;
                    }
                    map[pos] = HighlightCategory::String;
                    ++pos;
                }
                continue;
            }

            // Numbers
            if (isDigit(ch))
            {
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Identifiers / keywords / Verb-Noun cmdlets
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && (isIdentChar(line[pos]) || line[pos] == '-'))
                    ++pos;
                auto const word = line.substr(start, pos - start);
                std::array<char, 64> buf {};
                auto const lower = toLowerInto(word, buf);
                if (isInSortedArray(PowershellKeywords, lower))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                else if (word.contains('-'))
                    fillRange(map, start, pos - start, HighlightCategory::Function);
                continue;
            }

            // Operators
            if (isOperatorChar(ch) || ch == '@')
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }

            // Punctuation
            if (isPunctuationChar(ch))
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), state };
    }

    // -------------------------------------------------------------------------
    // Windows CMD / batch highlighter
    // -------------------------------------------------------------------------

    /// @brief Highlights a line of Windows CMD / batch script.
    auto highlightBatch(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Inspect the first non-blank token for line-level constructs.
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;

        // :: comment (a label that is conventionally used as a comment)
        if (pos + 1 < len && line[pos] == ':' && line[pos + 1] == ':')
        {
            fillRange(map, pos, len - pos, HighlightCategory::Comment);
            return { std::move(map), HighlightState::Normal };
        }

        // Label definition: :label
        if (pos < len && line[pos] == ':')
        {
            fillRange(map, pos, len - pos, HighlightCategory::Function);
            return { std::move(map), HighlightState::Normal };
        }

        // REM comment line (case-insensitive, must be its own token)
        {
            auto wordEnd = pos;
            while (wordEnd < len && isIdentChar(line[wordEnd]))
                ++wordEnd;
            auto const first = line.substr(pos, wordEnd - pos);
            std::array<char, 4> rbuf {};
            auto const lowerFirst = toLowerInto(first, rbuf);
            if (lowerFirst == "rem" && (wordEnd == len || line[wordEnd] == ' ' || line[wordEnd] == '\t'))
            {
                fillRange(map, pos, len - pos, HighlightCategory::Comment);
                return { std::move(map), HighlightState::Normal };
            }
        }

        while (pos < len)
        {
            auto const ch = line[pos];

            // Environment / parameter variable: %VAR%, %1, %%i
            if (ch == '%')
            {
                auto const start = pos;
                ++pos;
                if (pos < len && line[pos] == '%') // %%i loop variable
                {
                    ++pos;
                    if (pos < len && isIdentChar(line[pos]))
                        ++pos;
                }
                else if (pos < len && isDigit(line[pos])) // %0 … %9 positional parameter
                {
                    ++pos;
                }
                else // %VAR%
                {
                    while (pos < len && line[pos] != '%')
                        ++pos;
                    if (pos < len)
                        ++pos; // closing '%'
                }
                fillRange(map, start, pos - start, HighlightCategory::Variable);
                continue;
            }

            // Delayed-expansion variable: !VAR!
            if (ch == '!' && pos + 1 < len && isIdentChar(line[pos + 1]))
            {
                auto const start = pos;
                ++pos;
                while (pos < len && line[pos] != '!')
                    ++pos;
                if (pos < len)
                    ++pos; // closing '!'
                fillRange(map, start, pos - start, HighlightCategory::Variable);
                continue;
            }

            // Strings
            if (ch == '"')
            {
                map[pos] = HighlightCategory::String;
                ++pos;
                while (pos < len)
                {
                    map[pos] = HighlightCategory::String;
                    if (line[pos] == '"')
                    {
                        ++pos;
                        break;
                    }
                    ++pos;
                }
                continue;
            }

            // Numbers
            if (isDigit(ch))
            {
                auto const numLen = scanNumber(line, pos);
                fillRange(map, pos, numLen, HighlightCategory::Number);
                pos += numLen;
                continue;
            }

            // Identifiers / keywords (case-insensitive)
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                auto const start = pos;
                while (pos < len && isIdentChar(line[pos]))
                    ++pos;
                auto const word = line.substr(start, pos - start);
                std::array<char, 32> buf {};
                auto const lower = toLowerInto(word, buf);
                if (isInSortedArray(CmdKeywords, lower))
                    fillRange(map, start, pos - start, HighlightCategory::Keyword);
                continue;
            }

            // Operators / redirection: | & > < ^ = etc.
            if (isOperatorChar(ch))
            {
                map[pos] = HighlightCategory::Operator;
                ++pos;
                continue;
            }

            // Punctuation
            if (isPunctuationChar(ch))
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                continue;
            }

            ++pos;
        }

        return { std::move(map), HighlightState::Normal };
    }

    // -------------------------------------------------------------------------
    // XML highlighter
    // -------------------------------------------------------------------------

    /// @brief Highlights a line of XML, tracking <!-- … --> comments via state.
    auto highlightXml(std::string_view line, HighlightState state) -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        // Continue a <!-- … --> comment carried over from the previous line.
        if (state == HighlightState::XmlComment)
        {
            while (pos < len)
            {
                if (pos + 2 < len && line[pos] == '-' && line[pos + 1] == '-' && line[pos + 2] == '>')
                {
                    fillRange(map, pos, 3, HighlightCategory::Comment);
                    pos += 3;
                    state = HighlightState::Normal;
                    break;
                }
                map[pos] = HighlightCategory::Comment;
                ++pos;
            }
            if (state == HighlightState::XmlComment)
                return { std::move(map), state };
        }

        while (pos < len)
        {
            auto const ch = line[pos];

            // Comment: <!-- … -->
            if (ch == '<' && line.substr(pos).starts_with("<!--"))
            {
                auto const start = pos;
                pos += 4;
                state = HighlightState::XmlComment;
                while (pos < len)
                {
                    if (pos + 2 < len && line[pos] == '-' && line[pos + 1] == '-' && line[pos + 2] == '>')
                    {
                        pos += 3;
                        state = HighlightState::Normal;
                        break;
                    }
                    ++pos;
                }
                fillRange(map, start, pos - start, HighlightCategory::Comment);
                continue;
            }

            // CDATA section: <![CDATA[ … ]]>
            if (ch == '<' && line.substr(pos).starts_with("<![CDATA["))
            {
                auto const start = pos;
                pos += 9;
                while (
                    pos < len
                    && (pos + 2 >= len || line[pos] != ']' || line[pos + 1] != ']' || line[pos + 2] != '>'))
                    ++pos;
                if (pos + 2 < len)
                    pos += 3; // closing ]]>
                else
                    pos = len;
                fillRange(map, start, pos - start, HighlightCategory::String);
                continue;
            }

            // Processing instruction: <?xml … ?>
            if (ch == '<' && pos + 1 < len && line[pos + 1] == '?')
            {
                auto const start = pos;
                pos += 2;
                while (pos < len && (pos + 1 >= len || line[pos] != '?' || line[pos + 1] != '>'))
                    ++pos;
                if (pos + 1 < len)
                    pos += 2; // closing ?>
                else
                    pos = len;
                fillRange(map, start, pos - start, HighlightCategory::Preprocessor);
                continue;
            }

            // Declaration: <!DOCTYPE …>
            if (ch == '<' && pos + 1 < len && line[pos + 1] == '!')
            {
                auto const start = pos;
                while (pos < len && line[pos] != '>')
                    ++pos;
                if (pos < len)
                    ++pos;
                fillRange(map, start, pos - start, HighlightCategory::Preprocessor);
                continue;
            }

            // Element tag: <tag …>, </tag>, <tag/>
            if (ch == '<')
            {
                map[pos] = HighlightCategory::Punctuation;
                ++pos;
                if (pos < len && line[pos] == '/')
                {
                    map[pos] = HighlightCategory::Punctuation;
                    ++pos;
                }
                // Tag name
                auto const nameStart = pos;
                while (
                    pos < len
                    && (isIdentChar(line[pos]) || line[pos] == '-' || line[pos] == ':' || line[pos] == '.'))
                    ++pos;
                fillRange(map, nameStart, pos - nameStart, HighlightCategory::Keyword);
                // Attributes until the closing '>'
                while (pos < len && line[pos] != '>')
                {
                    auto const c = line[pos];
                    if (c == '"' || c == '\'')
                    {
                        auto const quote = c;
                        map[pos] = HighlightCategory::String;
                        ++pos;
                        while (pos < len)
                        {
                            map[pos] = HighlightCategory::String;
                            if (line[pos] == quote)
                            {
                                ++pos;
                                break;
                            }
                            ++pos;
                        }
                        continue;
                    }
                    if (c == '=' || c == '/')
                    {
                        map[pos] = c == '=' ? HighlightCategory::Operator : HighlightCategory::Punctuation;
                        ++pos;
                        continue;
                    }
                    if (isIdentChar(c) || c == '-' || c == ':')
                    {
                        auto const attrStart = pos;
                        while (pos < len
                               && (isIdentChar(line[pos]) || line[pos] == '-' || line[pos] == ':'
                                   || line[pos] == '.'))
                            ++pos;
                        fillRange(map, attrStart, pos - attrStart, HighlightCategory::Variable);
                        continue;
                    }
                    ++pos;
                }
                if (pos < len && line[pos] == '>')
                {
                    map[pos] = HighlightCategory::Punctuation;
                    ++pos;
                }
                continue;
            }

            // Entity reference: &amp; &#10; …
            if (ch == '&')
            {
                auto const start = pos;
                ++pos;
                while (pos < len && line[pos] != ';' && (isIdentChar(line[pos]) || line[pos] == '#'))
                    ++pos;
                if (pos < len && line[pos] == ';')
                    ++pos;
                fillRange(map, start, pos - start, HighlightCategory::Constructor);
                continue;
            }

            ++pos;
        }

        return { std::move(map), state };
    }

    // -------------------------------------------------------------------------
    // INI / .editorconfig highlighter
    // -------------------------------------------------------------------------

    /// @brief Highlights a line of INI / .editorconfig configuration.
    auto highlightIni(std::string_view line, HighlightState /*state*/)
        -> std::pair<HighlightMap, HighlightState>
    {
        auto const len = line.size();
        auto map = HighlightMap(len, HighlightCategory::Default);
        auto pos = std::size_t { 0 };

        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;
        if (pos >= len)
            return { std::move(map), HighlightState::Normal };

        // Comment: ; or #
        if (line[pos] == ';' || line[pos] == '#')
        {
            fillRange(map, pos, len - pos, HighlightCategory::Comment);
            return { std::move(map), HighlightState::Normal };
        }

        // Section header: [section] (also .editorconfig glob sections)
        if (line[pos] == '[')
        {
            auto const start = pos;
            while (pos < len && line[pos] != ']')
                ++pos;
            if (pos < len)
                ++pos; // include ']'
            fillRange(map, start, pos - start, HighlightCategory::Keyword);
            return { std::move(map), HighlightState::Normal };
        }

        // key = value  /  key : value
        auto sepPos = std::string_view::npos;
        for (auto const p: std::views::iota(pos, std::max(pos, len)))
        {
            if (line[p] == '=' || line[p] == ':')
            {
                sepPos = p;
                break;
            }
        }
        if (sepPos == std::string_view::npos)
        {
            // Bare token — treat as a key.
            fillRange(map, pos, len - pos, HighlightCategory::Variable);
            return { std::move(map), HighlightState::Normal };
        }

        // Key (trim trailing whitespace before the separator).
        auto keyEnd = sepPos;
        while (keyEnd > pos && (line[keyEnd - 1] == ' ' || line[keyEnd - 1] == '\t'))
            --keyEnd;
        fillRange(map, pos, keyEnd - pos, HighlightCategory::Variable);
        map[sepPos] = HighlightCategory::Operator;

        // Value (skip leading whitespace) — coloured as a string for visibility.
        auto valStart = sepPos + 1;
        while (valStart < len && (line[valStart] == ' ' || line[valStart] == '\t'))
            ++valStart;
        fillRange(map, valStart, len - valStart, HighlightCategory::String);

        return { std::move(map), HighlightState::Normal };
    }

} // namespace

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

namespace
{
    /// @brief How many languages one registry can hand ids out for.
    constexpr auto RegisteredLanguageCapacity =
        std::size_t { std::numeric_limits<std::uint8_t>::max() } + 1 - FirstRegisteredLanguageId;

    /// @brief The id of the @p index-th registered language of a registry.
    constexpr auto registeredLanguageId(std::size_t index) noexcept -> LanguageId
    {
        return static_cast<LanguageId>(static_cast<std::uint8_t>(FirstRegisteredLanguageId + index));
    }

    /// @brief The index into a registry's languages of a registered id.
    constexpr auto registeredLanguageIndex(LanguageId language) noexcept -> std::size_t
    {
        return static_cast<std::size_t>(std::to_underlying(language)) - FirstRegisteredLanguageId;
    }

    /// @brief Highlights one line with a built-in language; any other id yields plain text.
    auto highlightBuiltin(std::string_view line, LanguageId language, HighlightState state)
        -> std::pair<HighlightMap, HighlightState>
    {
        if (line.empty() || language == LanguageId::None)
            return { HighlightMap {}, state };

        switch (language)
        {
            case LanguageId::Cpp: return highlightCpp(line, state);
            case LanguageId::Python: return highlightPython(line, state);
            case LanguageId::Bash: return highlightBash(line, state);
            case LanguageId::CMake: return highlightCMake(line, state);
            case LanguageId::Markdown: return highlightMarkdown(line, state);
            case LanguageId::Json: return highlightJson(line, state);
            case LanguageId::Yaml: return highlightYaml(line, state);
            case LanguageId::GitDiff: return highlightGitDiff(line, state);
            case LanguageId::Assembly: return highlightAssembly(line, state);
            case LanguageId::PowerShell: return highlightPowerShell(line, state);
            case LanguageId::Cmd: return highlightBatch(line, state);
            case LanguageId::Xml: return highlightXml(line, state);
            case LanguageId::Ini: return highlightIni(line, state);
            case LanguageId::None:
            case LanguageId::Last: break;
        }

        // A registered id belongs to a registry, which dispatches it; here it is plain text.
        return { HighlightMap(line.size(), HighlightCategory::Default), state };
    }
} // namespace

auto detectLanguageFromPath(std::string_view filePath, SyntaxHighlighterRegistry const* registry)
    -> LanguageId
{
    // Reduce to the basename so directory components can't fool the match.
    auto const slash = filePath.find_last_of("/\\");
    auto const fileName = slash == std::string_view::npos ? filePath : filePath.substr(slash + 1);

    // Well-known file names take precedence over extension detection.
    if (auto const byName = lookupLanguage(FilenameLanguageTable, fileName); byName != LanguageId::None)
        return byName;

    // Fall back to extension detection.
    auto const dotPos = fileName.rfind('.');
    if (dotPos == std::string_view::npos)
        return LanguageId::None;

    return detectLanguageFromExtension(fileName.substr(dotPos), registry);
}

auto highlightLine(std::string_view line,
                   LanguageId language,
                   HighlightState state,
                   SyntaxHighlighterRegistry const* registry) -> std::pair<HighlightMap, HighlightState>
{
    if (registry != nullptr)
        return registry->highlightLine(line, language, state);

    return highlightBuiltin(line, language, state);
}

// -------------------------------------------------------------------------
// SyntaxHighlighterRegistry
// -------------------------------------------------------------------------

auto SyntaxHighlighterRegistry::registerLanguage(LanguageDefinition definition)
    -> std::expected<LanguageId, LanguageRegistrationFailure>
{
    using enum LanguageRegistrationError;

    auto const refuse = [](LanguageRegistrationError error, std::string token) {
        return std::unexpected(LanguageRegistrationFailure { .error = error, .token = std::move(token) });
    };

    if (definition.name.empty())
        return refuse(EmptyName, {});

    if (!definition.highlight)
        return refuse(NoHighlighter, definition.name);

    if (_languages.size() >= RegisteredLanguageCapacity)
        return refuse(CapacityReached, definition.name);

    // Every check runs before anything is stored, so a refused definition leaves the registry
    // exactly as it was.
    if (std::ranges::find(BuiltinLanguageTable, definition.name, &LanguageName::name)
            != BuiltinLanguageTable.end()
        || std::ranges::find(_languages, definition.name, &LanguageDefinition::name) != _languages.end())
        return refuse(NameInUse, definition.name);

    for (auto const& extension: definition.extensions)
    {
        // detectLanguageFromPath() only ever looks an extension up from the last dot of a
        // basename onwards, so one that does not start with a dot could never match anything:
        // it would register and sit dead. Refuse it where the mistake is visible.
        if (extension.size() < 2 || !extension.starts_with('.'))
            return refuse(MalformedToken, extension);

        if (detectFromExtension(extension) != LanguageId::None)
            return refuse(TokenInUse, extension);

        // A file name is matched whole and before any extension, so registering `.editorconfig`
        // as an extension would be shadowed for the one path that spells it exactly. That is the
        // same collision as any other, and gets the same answer.
        if (lookupLanguage(FilenameLanguageTable, extension) != LanguageId::None)
            return refuse(TokenInUse, extension);
    }

    for (auto const& fenceTag: definition.fenceTags)
    {
        // An empty tag is what a bare ``` fence yields, which means "no language"; a registered
        // empty tag would capture every unlabelled code block in every document.
        if (fenceTag.empty())
            return refuse(MalformedToken, fenceTag);

        if (detectFromFenceTag(fenceTag) != LanguageId::None)
            return refuse(TokenInUse, fenceTag);
    }

    auto const language = registeredLanguageId(_languages.size());
    _languages.push_back(std::move(definition));
    return language;
}

auto SyntaxHighlighterRegistry::find(std::string_view name) const noexcept -> LanguageId
{
    if (auto const builtin = std::ranges::find(BuiltinLanguageTable, name, &LanguageName::name);
        builtin != BuiltinLanguageTable.end())
        return builtin->language;

    auto const it = std::ranges::find(_languages, name, &LanguageDefinition::name);
    if (it == _languages.end())
        return LanguageId::None;

    return registeredLanguageId(static_cast<std::size_t>(std::ranges::distance(_languages.begin(), it)));
}

auto SyntaxHighlighterRegistry::name(LanguageId language) const noexcept -> std::string_view
{
    if (!isRegisteredLanguage(language))
    {
        auto const index = static_cast<std::size_t>(std::to_underlying(language));
        return index < BuiltinLanguageTable.size() ? BuiltinLanguageTable[index].name : std::string_view {};
    }

    auto const index = registeredLanguageIndex(language);
    return index < _languages.size() ? std::string_view { _languages[index].name } : std::string_view {};
}

auto SyntaxHighlighterRegistry::registeredCount() const noexcept -> std::size_t
{
    return _languages.size();
}

auto SyntaxHighlighterRegistry::detectFromExtension(std::string_view ext) const noexcept -> LanguageId
{
    if (auto const builtin = lookupLanguage(ExtensionLanguageTable, ext); builtin != LanguageId::None)
        return builtin;

    return findByToken(ext, &LanguageDefinition::extensions);
}

auto SyntaxHighlighterRegistry::detectFromFenceTag(std::string_view tag) const noexcept -> LanguageId
{
    if (auto const builtin = lookupLanguage(FenceTagLanguageTable, tag); builtin != LanguageId::None)
        return builtin;

    return findByToken(tag, &LanguageDefinition::fenceTags);
}

auto SyntaxHighlighterRegistry::detectFromPath(std::string_view filePath) const -> LanguageId
{
    return core::tui::detectLanguageFromPath(filePath, this);
}

auto SyntaxHighlighterRegistry::highlightLine(std::string_view line,
                                              LanguageId language,
                                              HighlightState state) const
    -> std::pair<HighlightMap, HighlightState>
{
    if (!isRegisteredLanguage(language))
        return highlightBuiltin(line, language, state);

    if (line.empty())
        return { HighlightMap {}, state };

    auto const index = registeredLanguageIndex(language);
    if (index >= _languages.size())
        return { HighlightMap(line.size(), HighlightCategory::Default), state };

    return _languages[index].highlight(line, state);
}

auto SyntaxHighlighterRegistry::findByToken(
    std::string_view token, std::vector<std::string> LanguageDefinition::* tokens) const noexcept
    -> LanguageId
{
    for (auto const index: std::views::iota(std::size_t { 0 }, _languages.size()))
    {
        auto const& candidates = _languages[index].*tokens;
        if (std::ranges::find(candidates, token) != candidates.end())
            return registeredLanguageId(index);
    }

    return LanguageId::None;
}

auto categoryColor(HighlightCategory cat, Theme const& theme) -> RgbColor
{
    return categoryColorFromIndex(static_cast<int>(cat), theme.syntaxColors);
}

void renderHighlightedLine(TerminalOutput& output,
                           std::string_view text,
                           HighlightMap const& highlights,
                           Style baseStyle,
                           Theme const& theme)
{
    if (text.empty())
        return;

    // Handle case where highlights map doesn't cover the full text (UTF-8 safety)
    auto const hlSize = highlights.size();

    auto pos = std::size_t { 0 };
    while (pos < text.size())
    {
        auto const cat = (pos < hlSize) ? highlights[pos] : HighlightCategory::Default;

        // Find the end of the span with the same category
        auto spanEnd = pos + 1;
        while (spanEnd < text.size() && spanEnd < hlSize && highlights[spanEnd] == cat)
            ++spanEnd;
        // Extend through any remaining bytes if we're past the highlight map
        if (spanEnd >= hlSize && pos < hlSize)
        {
            while (spanEnd < text.size())
                ++spanEnd;
        }

        auto style = baseStyle;
        style.fg = categoryColor(cat, theme);

        output.writeText(text.substr(pos, spanEnd - pos), style);
        pos = spanEnd;
    }
}

auto renderHighlightedLineToString(std::string_view text, HighlightMap const& highlights, Theme const& theme)
    -> std::string
{
    if (text.empty())
        return {};

    auto result = std::string {};
    result.reserve(text.size() * 2); // rough estimate for ANSI overhead

    auto const hlSize = highlights.size();
    auto pos = std::size_t { 0 };

    while (pos < text.size())
    {
        auto const cat = (pos < hlSize) ? highlights[pos] : HighlightCategory::Default;

        // Find the end of the span with the same category
        auto spanEnd = pos + 1;
        while (spanEnd < text.size() && spanEnd < hlSize && highlights[spanEnd] == cat)
            ++spanEnd;
        if (spanEnd >= hlSize && pos < hlSize)
        {
            while (spanEnd < text.size())
                ++spanEnd;
        }

        auto const color = categoryColor(cat, theme);
        result += std::format("\033[38;2;{};{};{}m", color.r, color.g, color.b);
        result.append(text.substr(pos, spanEnd - pos));

        pos = spanEnd;
    }

    result += "\033[m";
    return result;
}

} // namespace core::tui
