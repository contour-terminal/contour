#! /usr/bin/env python3
"""Keeps the agent orchestrator's guest code portable to wasm32-wasip1.

The orchestrator's core, view and guest driver (extensions/orchestrator/{core,view,guest}) are built for
the host and also as a WebAssembly guest compiled with -fno-exceptions -fno-rtti and no OS access. A
construct that needs exceptions, RTTI, a run-time format string, a host stream, a thread, a locale or an
ambient clock, random source or environment compiles on the host and only breaks the guest build, or
worse, links and traps at run time. This gate fails on any of them, one rule per row of RULES, and prints
every offender as `path:line: rule: message`.

Comments and string and character literals (raw strings included) are blanked out before matching, so a
rule named in a comment or a message does not offend; the header name of an #include is kept, since the
header rules match on it. Test code (*_test.cpp and test_main.cpp) is exempt: it runs on the host only.
`--self-test` checks every rule against an offending and a clean inline sample and needs no tree. Python
rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The directories whose sources must stay guest-portable, repository-relative.
GUEST_DIRS = tuple(Path("extensions/orchestrator") / name for name in ("core", "view", "guest"))

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}

# The host-only files inside GUEST_DIRS.
EXEMPT_NAME_SUFFIX = "_test.cpp"
EXEMPT_NAMES = {"test_main.cpp"}

# (name, pattern, message): matched against each line once comments and literals are blanked out.
RULES: tuple[tuple[str, re.Pattern[str], str], ...] = (
    ("throw", re.compile(r"\bthrow\b"), "guest code is built with -fno-exceptions; return std::expected instead"),
    ("try", re.compile(r"\btry\b"), "guest code is built with -fno-exceptions; there is nothing to catch"),
    ("catch", re.compile(r"\bcatch\b"), "guest code is built with -fno-exceptions; there is nothing to catch"),
    ("at", re.compile(r"(?:\.|->)\s*at\s*\("), "at() throws on a bad index; check the bound and use operator[]"),
    (
        "value",
        re.compile(r"(?:\.|->)\s*value\s*\(\s*\)"),
        "value() throws on an empty optional or expected; test it and use operator* or value_or()",
    ),
    (
        "vformat",
        re.compile(r"\b(?:vformat(?:_to)?|runtime_format)\s*\("),
        "a run-time format string throws on a malformed one; use std::format with a literal",
    ),
    (
        "stoi",
        re.compile(r"\bsto(?:i|l|ll|ul|ull|f|d|ld)\s*\("),
        "the std::stoi family throws; use std::from_chars",
    ),
    (
        "include",
        re.compile(r'#\s*include\s*[<"](?:iostream|fstream|filesystem|thread|mutex|future|regex|locale|sstream)[>"]'),
        "a host-only standard header; the guest has no streams, files, threads or locales",
    ),
    ("dynamic_cast", re.compile(r"\bdynamic_cast\b"), "guest code is built with -fno-rtti"),
    ("typeid", re.compile(r"\btypeid\b"), "guest code is built with -fno-rtti"),
    (
        "clock",
        re.compile(
            r"_clock\s*(?:::|(?:\{\s*\}|\(\s*\))\s*\.)\s*now\b"
            r"|(?<![\w.>:])(?:std\s*::\s*|::)?(?:time|clock|clock_gettime|gettimeofday|timespec_get)\s*\("
        ),
        "reads the clock directly; read time through the injected interface and name its method now()",
    ),
    (
        "random",
        re.compile(
            r"\brandom_device\b"
            r"|(?<![\w.>:])(?:std\s*::\s*|::)?"
            r"(?:s?rand|s?random|[delmnj]rand48|srand48|seed48|arc4random\w*|getrandom|getentropy)\s*\("
        ),
        "reads a random source directly; take randomness from an injected source",
    ),
    (
        "environment",
        re.compile(r"\b(?:(?:secure_getenv|getenv|_wgetenv|_dupenv_s)\s*\(|environ\b)"),
        "reads the environment directly; take the value from the host through the interface",
    ),
)

# The rule that matches on an #include's header name, and that name: what the other rules must not see.
INCLUDE_RULE = "include"
HEADER_NAME = re.compile(r'^(\s*#\s*include\s*)(?:<[^>]*>|"[^"]*")')
# An #include directive's header name, kept verbatim by the stripper.
INCLUDE_DIRECTIVE = re.compile(r'[ \t]*#[ \t]*include[ \t]*(?:<[^>\n]*>|"[^"\n]*")')
# The encoding prefixes a character or string literal may carry, and those that make a string raw.
LITERAL_PREFIXES = {"", "u8", "u", "U", "L"}
RAW_PREFIXES = {"R", "u8R", "uR", "UR", "LR"}


def blank(text: str) -> str:
    """`text` with every character but a newline replaced by a space, so line numbers survive."""
    return "".join("\n" if character == "\n" else " " for character in text)


def literal_prefix(text: str, quote: int) -> str | None:
    """The identifier glued to the quote at `quote` (its literal prefix), or None if it begins at a word."""
    start = quote
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
        start -= 1
    prefix = text[start:quote]
    return prefix if prefix in LITERAL_PREFIXES or prefix in RAW_PREFIXES else None


def strip_code(text: str) -> str:
    """`text` with comments and string and character literals blanked out, newlines kept.

    A literal becomes an empty pair of its quotes; a comment becomes blanks. An #include's header name is
    kept. A `'` glued to a word that is not an encoding prefix is a digit separator (1'000) and stays.
    """
    out: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        if i == 0 or text[i - 1] == "\n":
            directive = INCLUDE_DIRECTIVE.match(text, i)
            if directive:
                out.append(directive.group(0))
                i = directive.end()
                continue
        character = text[i]
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = length if end < 0 else end
            out.append(blank(text[i:end]))
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = length if end < 0 else end + 2
            out.append(blank(text[i:end]))
            i = end
        elif character == '"':
            prefix = literal_prefix(text, i)
            if prefix in RAW_PREFIXES:
                open_paren = text.find("(", i + 1)
                delimiter = text[i + 1 : open_paren] if open_paren >= 0 else ""
                close = text.find(")" + delimiter + '"', open_paren + 1) if open_paren >= 0 else -1
                end = length if close < 0 else close + len(delimiter) + 2
            else:
                end = i + 1
                while end < length and text[end] not in '"\n':
                    end += 2 if text[end] == "\\" else 1
                end = min(end + 1, length)
            out.append('"' + blank(text[i + 1 : end - 1]) + '"')
            i = end
        elif character == "'" and literal_prefix(text, i) in LITERAL_PREFIXES:
            end = i + 1
            while end < length and text[end] not in "'\n":
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
            out.append("'" + blank(text[i + 1 : end - 1]) + "'")
            i = end
        else:
            out.append(character)
            i += 1
    return "".join(out)


def violations_in(text: str) -> list[tuple[int, str]]:
    """The (line number, rule name) pairs `text` offends, in line order.

    The rule INCLUDE_RULE sees an #include's header name; every other rule sees it blanked, so that a
    header path such as "detail/try.hpp" offends nothing. Lines are split on "\n" alone: splitlines() also
    splits on a form feed and other separators the compiler counts as no line, and would shift the numbers.
    """
    violations: list[tuple[int, str]] = []
    for number, line in enumerate(strip_code(text).split("\n"), start=1):
        code = HEADER_NAME.sub(lambda directive: directive.group(1), line)
        violations.extend(
            (number, name) for name, pattern, _ in RULES if pattern.search(line if name == INCLUDE_RULE else code)
        )
    return violations


def is_exempt(relative: Path) -> bool:
    """Whether `relative` is host-only test code."""
    return relative.name.endswith(EXEMPT_NAME_SUFFIX) or relative.name in EXEMPT_NAMES


def checked_files(repo_root: Path = REPO_ROOT) -> list[Path]:
    """The guest sources in the tree at `repo_root`, sorted, repository-relative."""
    found = [
        path.relative_to(repo_root)
        for directory in GUEST_DIRS
        for path in (repo_root / directory).rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    ]
    return sorted(path for path in found if not is_exempt(path))


def scan(repo_root: Path = REPO_ROOT) -> list[str]:
    """Every violation in the tree at `repo_root`, as `path:line: rule: message`."""
    messages = {name: message for name, _, message in RULES}
    violations: list[str] = []
    for relative in checked_files(repo_root):
        text = (repo_root / relative).read_text(encoding="utf-8", errors="replace")
        violations.extend(
            f"{relative.as_posix()}:{number}: {name}: {messages[name]}" for number, name in violations_in(text)
        )
    return violations


def self_test() -> int:
    """Checks every rule against offending and clean samples, the stripper, the skip list and the walk."""
    cases: list[tuple[str, str, bool]] = [
        # (sample, rule, whether it offends): an offending sample must be reported under its rule; a clean
        # one, a neighbour of that rule that could be caught by accident, must be reported under no rule.
        ("throw std::runtime_error{};", "throw", True),
        ("try {", "try", True),
        ("catch (...)", "catch", True),
        ("v.at(0)", "at", True),
        ("p->at(0)", "at", True),
        ("x.value()", "value", True),
        ("p->value()", "value", True),
        ("dynamic_cast<T*>(p)", "dynamic_cast", True),
        ("typeid(x)", "typeid", True),
        ("std::stoi(s)", "stoi", True),
        ("std::stol(s)", "stoi", True),
        ("std::stoll(s)", "stoi", True),
        ("std::stoul(s)", "stoi", True),
        ("std::stoull(s)", "stoi", True),
        ("std::stof(s)", "stoi", True),
        ("std::stod(s)", "stoi", True),
        ("std::stold(s)", "stoi", True),
        ("std::vformat(f, a)", "vformat", True),
        ("std::vformat_to(out, f, a)", "vformat", True),
        ("std::format(std::runtime_format(f), a)", "vformat", True),
        ("#include <iostream>", "include", True),
        ("#include <fstream>", "include", True),
        ("#include <filesystem>", "include", True),
        ("#include <thread>", "include", True),
        ("#include <mutex>", "include", True),
        ("#include <future>", "include", True),
        ("#include <regex>", "include", True),
        ("#include <locale>", "include", True),
        ("#include <sstream>", "include", True),
        ('#  include "sstream"', "include", True),
        ("auto t = std::chrono::steady_clock::now();", "clock", True),
        ("auto t = std::chrono::system_clock::now();", "clock", True),
        ("auto t = high_resolution_clock::now();", "clock", True),
        ("clock_gettime(CLOCK_MONOTONIC, &ts);", "clock", True),
        ("std::random_device device;", "random", True),
        ("auto r = std::rand();", "random", True),
        ("auto home = getenv(\"HOME\");", "environment", True),
        ("auto home = std::getenv(\"HOME\");", "environment", True),
        ("auto home = secure_getenv(\"HOME\");", "environment", True),
        ("auto t = time(nullptr);", "clock", True),
        ("auto t = ::time(nullptr);", "clock", True),
        ("auto t = std::time(nullptr);", "clock", True),
        ("auto c = std::clock();", "clock", True),
        ("auto t = std::chrono::steady_clock{}.now();", "clock", True),
        ("auto t = std::chrono::steady_clock().now();", "clock", True),
        ("auto t = std::chrono::system_clock ( ) . now();", "clock", True),
        ("auto t = high_resolution_clock{ }.now();", "clock", True),
        # Deliberate: a method declared time() or clock() offends too; the injected interface names it now().
        ("virtual Time time() const = 0;", "clock", True),
        ("Clock& clock();", "clock", True),
        ("auto r = random();", "random", True),
        ("auto r = drand48();", "random", True),
        ("auto r = lrand48();", "random", True),
        ("srand48(seed);", "random", True),
        ("srandom(seed);", "random", True),
        # Clean: the brief's samples.
        ("// throw", "throw", False),
        ('"throw"', "throw", False),
        ("rethrow_hint", "throw", False),
        ("value_or(", "value", False),
        (".value_or(", "value", False),
        ("at_least(", "at", False),
        # Clean: the neighbours a rule could catch by accident.
        ("/* try { } catch (...) { throw; } */ int a;", "throw", False),
        ("/*\n * x.at(0)\n */\nint a;", "at", False),
        ('auto s = R"(throw x.value() typeid)";', "typeid", False),
        ('auto s = R"x(\n  )" throw "\n  std::getenv(\n)x";', "environment", False),
        ("auto c = '\"'; auto d = u8'x';", "throw", False),
        ("auto n = 1'000'000;", "throw", False),
        ("m.try_emplace(k, v);", "try", False),
        ("catchAll(handler);", "catch", False),
        ("flag.atomic(0);", "at", False),
        ("auto a = v.attach(0);", "at", False),
        ("using value_type = int;", "value", False),
        ("auto v = e.value_or(0);", "value", False),
        ("auto v = opt.value(fallback);", "value", False),
        ("static_cast<int>(x)", "dynamic_cast", False),
        ("std::type_identity<T> t;", "typeid", False),
        ("std::from_chars(first, last, n);", "stoi", False),
        ("store(x); stop(); auto s = stod_hint;", "stoi", False),
        ('std::format("{}", x)', "vformat", False),
        ("#include <locale_utils.hpp>", "include", False),
        ('#include "core/Thread.hpp"', "include", False),
        ("#include <string_view>", "include", False),
        ("#include <vector> // <thread> is host-only", "include", False),
        ('#include "core/environ.hpp"', "environment", False),
        ('#include "throw/Policy.hpp"', "throw", False),
        ('#include "detail/try.hpp"', "try", False),
        ("auto t = clock.now();", "clock", False),
        ("auto now = injectedClock();", "clock", False),
        ("auto t = timeout(5); auto r = runtime(); auto m = event.time(); auto c = clockRate(2);", "clock", False),
        ("auto t = Clock::time(); auto u = self->clock();", "clock", False),
        ("auto r = random.next(); auto b = brand(x); auto o = operand(y);", "random", False),
        ("auto r = rng.random(); auto s = Engine::random(); auto g = grand48(x);", "random", False),
        ("auto e = environment.lookup(\"HOME\");", "environment", False),
    ]
    failures: list[str] = []
    for sample, rule, offends in cases:
        found = {name for _, name in violations_in(sample)}
        if found if not offends else rule not in found:
            failures.append(f"{sample!r}: expected {rule if offends else 'clean'}, got {sorted(found) or 'clean'}")
    # Every rule is held to at least one offending and one clean sample.
    names = {name for name, _, _ in RULES}
    for offends in (True, False):
        covered = {rule for _, rule, sample_offends in cases if sample_offends == offends}
        if covered != names:
            kind = "offending" if offends else "clean"
            failures.append(f"{kind} samples cover {sorted(covered)}, not exactly the rules {sorted(names)}")
    # A form feed is no line break to the compiler, so it must not shift the reported line.
    lines = [number for number, _ in violations_in("int a;\f\nx.at(0);")]
    if lines != [2]:
        failures.append(f"line numbers across a form feed: expected [2], got {lines}")
    # A rule that is found must be reported on its own line, not on the line its literal or comment opened.
    lines = [number for number, _ in violations_in('auto s = R"(\n\n)";\nx.at(0);\n/*\n*/ throw;')]
    if lines != [4, 6]:
        failures.append(f"line numbers across a raw string and a block comment: expected [4, 6], got {lines}")
    exempt = [
        ("core/Utf8_test.cpp", True),
        ("core/test_main.cpp", True),
        ("core/Utf8.hpp", False),
        ("guest/main.cpp", False),
    ]
    failures += [f"is_exempt({path}) != {expected}" for path, expected in exempt if is_exempt(Path(path)) != expected]
    # The walk reports an offender under each guest directory, and skips test code and the other dirs.
    tree = {
        "extensions/orchestrator/core/A.hpp": True,
        "extensions/orchestrator/view/B.cpp": True,
        "extensions/orchestrator/guest/C.cpp": True,
        "extensions/orchestrator/core/A_test.cpp": False,
        "extensions/orchestrator/core/test_main.cpp": False,
        "extensions/orchestrator/proto/D.cpp": False,
        "extensions/orchestrator/harness/E.cpp": False,
    }
    with tempfile.TemporaryDirectory() as tmp:
        for path in tree:
            (Path(tmp) / path).parent.mkdir(parents=True, exist_ok=True)
            (Path(tmp) / path).write_text("int a;\nauto x = v.at(0);\n")
        reported = scan(Path(tmp))
    for path, expected in tree.items():
        if any(line.startswith(f"{path}:2: at:") for line in reported) != expected:
            failures.append(f"walk: {path} expected {'reported' if expected else 'skipped'}")
    for failure in failures:
        print(f"check-guest-portability self-test: FAILED: {failure}", file=sys.stderr)
    # The cases, the two coverage checks, the two line-number checks, the skip list and the walk.
    total = len(cases) + 4 + len(exempt) + len(tree)
    if failures:
        print(f"check-guest-portability self-test: {len(failures)} of {total} cases failed.", file=sys.stderr)
        return 1
    print(f"check-guest-portability self-test: {total} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline samples")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    files = checked_files()
    if not files:
        print("check-guest-portability: no guest source found; the gate would pass vacuously.", file=sys.stderr)
        return 1
    violations = scan()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(f"check-guest-portability: {len(violations)} violation(s).", file=sys.stderr)
        return 1
    print(f"check-guest-portability: {len(files)} guest files are portable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
