// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/HierarchicalContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>

using namespace vtbackend;
using namespace std::string_view_literals;

namespace
{

/// Decodes @p payload, requiring success, and keeps the scratch alive for the caller's views.
struct Decoded
{
    std::string scratch;
    ContextCommand command;

    explicit Decoded(std::string_view payload)
    {
        auto result = parseContextSequence(payload, scratch);
        REQUIRE(result.has_value());
        command = *result;
    }
};

/// The error @p payload is rejected with.
ContextParseError errorOf(std::string_view payload)
{
    auto scratch = std::string {};
    auto const result = parseContextSequence(payload, scratch);
    REQUIRE(!result.has_value());
    return result.error();
}

/// One unescaped value, or nullopt when the raw text is not a valid run of SAFE elements.
std::optional<std::string> unescaped(std::string_view raw)
{
    auto out = std::string {};
    if (!unescapeContextValue(raw, out))
        return std::nullopt;
    return out;
}

} // namespace

// {{{ the unescaper

TEST_CASE("HierarchicalContext.an escaped semicolon decodes", "[context]")
{
    CHECK(unescaped("a\\x3bb") == "a;b");
}

TEST_CASE("HierarchicalContext.an escaped backslash decodes", "[context]")
{
    CHECK(unescaped("a\\x5cb") == "a\\b");
}

TEST_CASE("HierarchicalContext.escapes are case-insensitive, as ABNF quoted strings are", "[context]")
{
    // RFC 5234 section 2.3. This is the grammar, not a lenience we invented.
    CHECK(unescaped("\\x3b") == ";");
    CHECK(unescaped("\\x3B") == ";");
    CHECK(unescaped("\\X3b") == ";");
    CHECK(unescaped("\\X3B") == ";");

    CHECK(unescaped("\\x5c") == "\\");
    CHECK(unescaped("\\x5C") == "\\");
    CHECK(unescaped("\\X5c") == "\\");
    CHECK(unescaped("\\X5C") == "\\");
}

TEST_CASE("HierarchicalContext.a stray backslash is rejected", "[context]")
{
    // A bare 0x5c is not a SAFE element in any position -- SAFE resumes at %x5d, one past it.
    CHECK(!unescaped("a\\b").has_value());
    CHECK(!unescaped("\\").has_value());
    CHECK(!unescaped("trailing\\").has_value());
}

TEST_CASE("HierarchicalContext.an unknown hex escape is rejected", "[context]")
{
    // Not a general \\xNN scheme: only the two the ABNF names are escapes.
    CHECK(!unescaped("\\x41").has_value());
    CHECK(!unescaped("\\x00").has_value());
    CHECK(!unescaped("\\y3b").has_value());
}

TEST_CASE("HierarchicalContext.a truncated escape at the end is rejected", "[context]")
{
    CHECK(!unescaped("\\x3").has_value());
    CHECK(!unescaped("\\x").has_value());
    CHECK(!unescaped("ok\\x5").has_value());
}

TEST_CASE("HierarchicalContext.bytes above 0x7f are kept verbatim", "[context]")
{
    // The specification deliberately departs from ECMA-48 to allow them, so a UTF-8 path survives.
    auto const utf8 = "/home/\xc3\xbcser/\xe6\x97\xa5\xe6\x9c\xac"sv;
    CHECK(unescaped(utf8) == std::string { utf8 });
}

TEST_CASE("HierarchicalContext.invalid UTF-8 is kept verbatim and not validated", "[context]")
{
    // The backend stores bytes: a Linux cwd is an arbitrary byte string, and rejecting one that is not
    // valid UTF-8 would discard a legitimate path. Transcoding is the frontend's job.
    auto const invalid = "\xff\xfe"sv;
    CHECK(unescaped(invalid) == std::string { invalid });
}

TEST_CASE("HierarchicalContext.control bytes are rejected", "[context]")
{
    CHECK(!unescaped("a\x01"
                     "b")
               .has_value());
    CHECK(!unescaped("a\x1f").has_value());
    // 0x7f in particular is REACHABLE: vtparser's OSC range ends at 0x7f, not 0x7e, so DEL is handed
    // to us even though the specification forbids it.
    CHECK(!unescaped("a\x7f").has_value());
}

TEST_CASE("HierarchicalContext.an empty value decodes to empty", "[context]")
{
    CHECK(unescaped("") == "");
}

// }}}
// {{{ ABNF acceptance

TEST_CASE("HierarchicalContext.a bare start is accepted", "[context]")
{
    // `STARTSEQ = OSC "3008;start=" CTXID *(";" STARTFIELD) ST` -- zero fields is legal.
    auto const decoded = Decoded { "start=abc" };
    CHECK(decoded.command.verb == ContextVerb::Start);
    CHECK(decoded.command.identifier == "abc");
    CHECK(decoded.command.present == ContextFields {});
}

TEST_CASE("HierarchicalContext.a bare end is accepted", "[context]")
{
    auto const decoded = Decoded { "end=abc" };
    CHECK(decoded.command.verb == ContextVerb::End);
    CHECK(decoded.command.identifier == "abc");
    CHECK(decoded.command.outcome == ContextOutcome {});
}

TEST_CASE("HierarchicalContext.every start field decodes", "[context]")
{
    auto const decoded =
        Decoded { "start=id;type=container;user=lennart;hostname=zeta;"
                  "machineid=3deb5353d3ba43d08201c136a47ead7b;bootid=d4a3d0fdf2e24fdea6d971ce73f4fbf2;"
                  "pid=1062862;pidfdid=1063162;comm=systemd-nspawn;cwd=/app;cmdline=ls -l;vm=myvm;"
                  "container=foobar;targetuser=root;targethost=example.com;sessionid=42" };
    auto const& command = decoded.command;

    CHECK(command.type == ContextType::Container);
    CHECK(command.user == "lennart");
    CHECK(command.hostname == "zeta");
    CHECK(command.machineId == "3deb5353d3ba43d08201c136a47ead7b");
    CHECK(command.bootId == "d4a3d0fdf2e24fdea6d971ce73f4fbf2");
    CHECK(command.pid == 1062862);
    CHECK(command.pidFdId == 1063162);
    CHECK(command.comm == "systemd-nspawn");
    CHECK(command.workingDirectory == "/app");
    CHECK(command.commandLine == "ls -l");
    CHECK(command.vm == "myvm");
    CHECK(command.container == "foobar");
    CHECK(command.targetUser == "root");
    CHECK(command.targetHost == "example.com");
    CHECK(command.sessionId == "42");

    // Every field the table names was recorded.
    for (auto const field: ContextFieldList)
        CHECK((command.present & field).any());
}

TEST_CASE("HierarchicalContext.every type enum decodes", "[context]")
{
    for (auto const type: ContextTypeList)
    {
        auto const payload = std::string { "start=id;type=" } + std::string { contextTypeName(type) };
        auto const decoded = Decoded { payload };
        CHECK(decoded.command.type == type);
    }
}

TEST_CASE("HierarchicalContext.every exit enum decodes", "[context]")
{
    for (auto const exit:
         { ContextExit::Success, ContextExit::Failure, ContextExit::Crash, ContextExit::Interrupt })
    {
        auto const payload = std::string { "end=id;exit=" } + std::string { contextExitName(exit) };
        auto const decoded = Decoded { payload };
        CHECK(decoded.command.outcome.exit == exit);
    }
}

TEST_CASE("HierarchicalContext.a symbolic signal decodes to its Linux number", "[context]")
{
    auto const decoded = Decoded { "end=id;exit=failure;status=139;signal=SIGSEGV" };
    CHECK(decoded.command.outcome.exit == ContextExit::Failure);
    CHECK(decoded.command.outcome.status == 139);
    CHECK(decoded.command.outcome.signal == ContextSignal::Segv);
    CHECK(decoded.command.outcome.asShellExitCode() == 139);
}

TEST_CASE("HierarchicalContext.field order is undefined so type may come last", "[context]")
{
    // The specification says so outright: "including that type= is specified at the very end or in the
    // middle".
    auto const decoded = Decoded { "start=id;cwd=/tmp;user=bob;type=shell" };
    CHECK(decoded.command.type == ContextType::Shell);
    CHECK(decoded.command.workingDirectory == "/tmp");
    CHECK(decoded.command.user == "bob");
}

TEST_CASE("HierarchicalContext.an escaped value round-trips through a field", "[context]")
{
    auto const decoded = Decoded { "start=id;cwd=/home/a\\x3bb\\x5cc" };
    CHECK(decoded.command.workingDirectory == "/home/a;b\\c");
}

TEST_CASE("HierarchicalContext.an escaped identifier decodes", "[context]")
{
    auto const decoded = Decoded { "start=a\\x3bb;type=shell" };
    CHECK(decoded.command.identifier == "a;b");
    CHECK(decoded.command.type == ContextType::Shell);
}

// }}}
// {{{ ABNF rejection and leniency

TEST_CASE("HierarchicalContext.a payload without a verb is rejected", "[context]")
{
    CHECK(errorOf("") == ContextParseError::MissingVerb);
    CHECK(errorOf("begin=abc") == ContextParseError::MissingVerb);
    CHECK(errorOf("abc") == ContextParseError::MissingVerb);
    CHECK(errorOf("START=abc") == ContextParseError::MissingVerb); // the verb itself is not ABNF-quoted
}

TEST_CASE("HierarchicalContext.an empty identifier is rejected", "[context]")
{
    // `CTXID = 1*64SAFE` -- at least one.
    CHECK(errorOf("start=") == ContextParseError::EmptyIdentifier);
    CHECK(errorOf("end=") == ContextParseError::EmptyIdentifier);
    CHECK(errorOf("start=;type=shell") == ContextParseError::EmptyIdentifier);
}

TEST_CASE("HierarchicalContext.an identifier over 64 bytes is rejected", "[context]")
{
    // The identifier is the SUBJECT of the sequence, not one of its fields, so the leniency clause
    // does not reach it: acting on a truncated key would attach metadata to the wrong context.
    CHECK(errorOf("start=" + std::string(65, 'a')) == ContextParseError::IdentifierTooLong);
    auto scratch = std::string {};
    CHECK(parseContextSequence("start=" + std::string(64, 'a'), scratch).has_value());
}

TEST_CASE("HierarchicalContext.identifier length is measured after unescaping", "[context]")
{
    // SAFE counts ELEMENTS, and ESCSEMICOLON is one of them spelled with four characters. Validating
    // the raw length would reject a legal id.
    auto scratch = std::string {};
    auto const sixtyFourSemicolons = std::string { "start=" };
    auto payload = sixtyFourSemicolons;
    for (auto i = 0; i < 64; ++i)
        payload += "\\x3b";
    auto const result = parseContextSequence(payload, scratch);
    REQUIRE(result.has_value());
    CHECK(result->identifier.size() == 64);

    payload += "\\x3b"; // one element too many
    CHECK(errorOf(payload) == ContextParseError::IdentifierTooLong);
}

TEST_CASE("HierarchicalContext.a malformed identifier is rejected", "[context]")
{
    CHECK(errorOf("start=a\\b") == ContextParseError::MalformedIdentifier);
    CHECK(errorOf("start=a\x7f") == ContextParseError::MalformedIdentifier);
}

TEST_CASE("HierarchicalContext.an oversized payload is rejected", "[context]")
{
    CHECK(errorOf("start=id;cwd=" + std::string(MaxContextPayloadLength, 'a'))
          == ContextParseError::PayloadTooLong);
}

TEST_CASE("HierarchicalContext.an unknown field is ignored and the rest survives", "[context]")
{
    // The specification: "if a sequence contains invalid fields, those fields should be ignored, but
    // the rest of the fields should still be used. In particular, unknown fields should be ignored."
    auto const decoded = Decoded { "start=id;nosuchfield=x;type=shell;alsonot=y;cwd=/tmp" };
    CHECK(decoded.command.type == ContextType::Shell);
    CHECK(decoded.command.workingDirectory == "/tmp");
}

TEST_CASE("HierarchicalContext.a bare token with no equals is ignored", "[context]")
{
    auto const decoded = Decoded { "start=id;justatoken;type=shell" };
    CHECK(decoded.command.type == ContextType::Shell);
}

TEST_CASE("HierarchicalContext.a field with a broken escape is dropped, not the sequence", "[context]")
{
    auto const decoded = Decoded { "start=id;cwd=/bad\\path;type=shell" };
    CHECK(decoded.command.type == ContextType::Shell);
    CHECK(!(decoded.command.present & ContextField::WorkingDirectory).any());
}

TEST_CASE("HierarchicalContext.an oversized field is discarded, not truncated", "[context]")
{
    // A truncated path names a REAL directory that is not the one meant, and something will eventually
    // open it. An absent field is honestly absent.
    auto const decoded =
        Decoded { "start=id;cwd=/" + std::string(MaxContextFieldLength, 'a') + ";type=shell" };
    CHECK(decoded.command.type == ContextType::Shell);
    CHECK(!(decoded.command.present & ContextField::WorkingDirectory).any());
    CHECK(decoded.command.workingDirectory.empty());
}

TEST_CASE("HierarchicalContext.a field of exactly the maximum length is accepted", "[context]")
{
    auto const decoded = Decoded { "start=id;cwd=" + std::string(MaxContextFieldLength, 'a') };
    CHECK((decoded.command.present & ContextField::WorkingDirectory).any());
    CHECK(decoded.command.workingDirectory.size() == MaxContextFieldLength);
}

TEST_CASE("HierarchicalContext.field length is measured after unescaping", "[context]")
{
    auto payload = std::string { "start=id;cwd=" };
    for (auto i = size_t {}; i < MaxContextFieldLength; ++i)
        payload += "\\x3b";
    auto const decoded = Decoded { payload };
    CHECK((decoded.command.present & ContextField::WorkingDirectory).any());
    CHECK(decoded.command.workingDirectory.size() == MaxContextFieldLength);
}

TEST_CASE("HierarchicalContext.an empty text field is discarded", "[context]")
{
    // `1*255SAFE`: empty is not a value.
    auto const decoded = Decoded { "start=id;user=;type=shell" };
    CHECK(!(decoded.command.present & ContextField::User).any());
    CHECK(decoded.command.type == ContextType::Shell);
}

TEST_CASE("HierarchicalContext.an empty cmdline is accepted because the ABNF allows it", "[context]")
{
    // `CMDLINE = "cmdline=" *255SAFE` -- deliberately different from every other text field, so honour
    // the difference. It is what tells "ran an empty command line" apart from "did not say".
    auto const decoded = Decoded { "start=id;cmdline=" };
    CHECK((decoded.command.present & ContextField::CommandLine).any());
    CHECK(decoded.command.commandLine.empty());
}

TEST_CASE("HierarchicalContext.a malformed ID128 is discarded", "[context]")
{
    CHECK(!(Decoded { "start=id;machineid=tooshort" }.command.present & ContextField::MachineId).any());
    CHECK(
        !(Decoded { "start=id;machineid=" + std::string(37, 'a') }.command.present & ContextField::MachineId)
             .any());
    CHECK(
        !(Decoded { "start=id;machineid=" + std::string(32, 'z') }.command.present & ContextField::MachineId)
             .any());
    CHECK(
        (Decoded { "start=id;bootid=" + std::string(32, 'a') }.command.present & ContextField::BootId).any());
    CHECK(
        (Decoded { "start=id;bootid=" + std::string(36, 'F') }.command.present & ContextField::BootId).any());
}

TEST_CASE("HierarchicalContext.a 21-digit pid is discarded", "[context]")
{
    // `UINT64 = 1*20DECIMAL`. Bounded BEFORE the digits are accumulated, or an absurdly long run would
    // wrap and land back on a value the caller accepts.
    CHECK(!(Decoded { "start=id;pid=" + std::string(21, '9') }.command.present & ContextField::Pid).any());
    CHECK((Decoded { "start=id;pid=" + std::string(20, '1') }.command.present & ContextField::Pid).any());
    CHECK(!(Decoded { "start=id;pid=12a" }.command.present & ContextField::Pid).any());
    CHECK(!(Decoded { "start=id;pid=-1" }.command.present & ContextField::Pid).any());
    CHECK(!(Decoded { "start=id;pid=" }.command.present & ContextField::Pid).any());
}

TEST_CASE("HierarchicalContext.an unknown type is ignored like any unknown field", "[context]")
{
    auto const decoded = Decoded { "start=id;type=nosuchtype;cwd=/tmp" };
    CHECK(decoded.command.type == ContextType::None);
    CHECK(!(decoded.command.present & ContextField::Type).any());
    CHECK(decoded.command.workingDirectory == "/tmp");
}

TEST_CASE("HierarchicalContext.a duplicate field takes the last value", "[context]")
{
    // Spec-silent; matches crispy::splitKeyValuePairs and the OSC 99 metadata loop, both last-wins.
    auto const decoded = Decoded { "start=id;user=first;user=second" };
    CHECK(decoded.command.user == "second");
}

TEST_CASE("HierarchicalContext.an unknown signal name is discarded", "[context]")
{
    auto const decoded = Decoded { "end=id;exit=crash;signal=SIGNOSUCH" };
    CHECK(decoded.command.outcome.exit == ContextExit::Crash);
    CHECK(decoded.command.outcome.signal == ContextSignal::None);
}

TEST_CASE("HierarchicalContext.a second verb later in the payload is ignored", "[context]")
{
    // The grammar puts the verb first; anything later is not a STARTFIELD.
    auto const decoded = Decoded { "start=id;type=shell;start=other" };
    CHECK(decoded.command.identifier == "id");
    CHECK(decoded.command.type == ContextType::Shell);
}

TEST_CASE("HierarchicalContext.start fields on an end sequence are ignored", "[context]")
{
    auto const decoded = Decoded { "end=id;cwd=/tmp;exit=success" };
    CHECK(decoded.command.outcome.exit == ContextExit::Success);
    CHECK(decoded.command.present == ContextFields {});
}

TEST_CASE("HierarchicalContext.end fields on a start sequence are ignored", "[context]")
{
    auto const decoded = Decoded { "start=id;exit=success;type=shell" };
    CHECK(decoded.command.outcome == ContextOutcome {});
    CHECK(decoded.command.type == ContextType::Shell);
}

// }}}
// {{{ the real traffic

TEST_CASE("HierarchicalContext.the systemd shell announcement decodes", "[context]")
{
    // Byte for byte what /etc/profile.d/80-systemd-osc-context.sh emits from PROMPT_COMMAND, including
    // the escaped cwd its __systemd_osc_context_escape produces.
    auto const decoded =
        Decoded { "start=8c1ba9a4-8b2f-4a1e-9c3d-2f5e7a9b1c4d;type=shell;"
                  "machineid=3deb5353d3ba43d08201c136a47ead7b;user=lennart;hostname=zeta;"
                  "bootid=d4a3d0fdf2e24fdea6d971ce73f4fbf2;pid=1062862;cwd=/home/lennart/pro\\x3bject" };

    CHECK(decoded.command.verb == ContextVerb::Start);
    CHECK(decoded.command.type == ContextType::Shell);
    CHECK(decoded.command.user == "lennart");
    CHECK(decoded.command.pid == 1062862);
    CHECK(decoded.command.workingDirectory == "/home/lennart/pro;ject");
    // No cmdline= at all: the systemd shim does not send one, so every 3008-derived block has none.
    CHECK(!(decoded.command.present & ContextField::CommandLine).any());
}

TEST_CASE("HierarchicalContext.the systemd failure report decodes", "[context]")
{
    auto const decoded = Decoded { "end=8c1ba9a4-8b2f-4a1e-9c3d-2f5e7a9b1c4d;exit=failure;status=139;"
                                   "signal=SIGSEGV" };
    CHECK(decoded.command.verb == ContextVerb::End);
    CHECK(decoded.command.outcome.exit == ContextExit::Failure);
    CHECK(decoded.command.outcome.signal == ContextSignal::Segv);
}

TEST_CASE("HierarchicalContext.the specification's container example decodes", "[context]")
{
    auto const decoded = Decoded { "start=bed86fab93af4328bbed0a1224af6d40;type=container;user=lennart;"
                                   "hostname=zeta;machineid=3deb5353d3ba43d08201c136a47ead7b;"
                                   "bootid=d4a3d0fdf2e24fdea6d971ce73f4fbf2;pid=1062862;pidfdid=1063162;"
                                   "comm=systemd-nspawn;container=foobar" };
    CHECK(decoded.command.type == ContextType::Container);
    CHECK(decoded.command.container == "foobar");
    CHECK(decoded.command.pidFdId == 1063162);
}

// }}}
// {{{ allocation discipline

TEST_CASE("HierarchicalContext.repeated decoding reuses the scratch buffer", "[context]")
{
    // The systemd hot path: the shell context is re-announced on EVERY prompt. Decoding must not
    // allocate once per prompt to store what we already have.
    auto scratch = std::string {};
    auto const payload = "start=shell-uuid;type=shell;machineid=3deb5353d3ba43d08201c136a47ead7b;"
                         "user=lennart;hostname=zeta;pid=1062862;cwd=/home/lennart"sv;

    REQUIRE(parseContextSequence(payload, scratch).has_value());
    auto const warmCapacity = scratch.capacity();

    for (auto i = 0; i < 100; ++i)
        REQUIRE(parseContextSequence(payload, scratch).has_value());

    CHECK(scratch.capacity() == warmCapacity);
}

TEST_CASE("HierarchicalContext.the decoded views point into the scratch buffer", "[context]")
{
    // What makes reuse safe: the scratch is reserved to the raw payload size up front, and an escape
    // only ever shrinks, so it never reallocates mid-decode and no view dangles.
    auto scratch = std::string {};
    auto const result = parseContextSequence("start=id;cwd=/a\\x3bb;user=bob", scratch);
    REQUIRE(result.has_value());

    auto const* const base = scratch.data();
    auto const* const end = base + scratch.size();
    CHECK(result->identifier.data() >= base);
    CHECK(result->identifier.data() < end);
    CHECK(result->workingDirectory.data() >= base);
    CHECK(result->workingDirectory.data() < end);
    CHECK(result->workingDirectory == "/a;b");
    CHECK(result->user == "bob");
}

// }}}
// {{{ diagnostics

TEST_CASE("HierarchicalContext.every parse error has a distinct name", "[context]")
{
    auto const names = std::array { toString(ContextParseError::MissingVerb),
                                    toString(ContextParseError::EmptyIdentifier),
                                    toString(ContextParseError::IdentifierTooLong),
                                    toString(ContextParseError::MalformedIdentifier),
                                    toString(ContextParseError::PayloadTooLong) };
    for (auto i = size_t {}; i < names.size(); ++i)
    {
        CHECK(!names[i].empty());
        CHECK(names[i] != "Unknown");
        for (auto j = i + 1; j < names.size(); ++j)
            CHECK(names[i] != names[j]);
    }
}

// }}}
