// SPDX-License-Identifier: Apache-2.0
//
// The sandbox seam: a system without a sandbox runs the host unsandboxed, and a step that failed is phrased
// from the one table both ends share.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/host/ContainerSandbox.hpp>
#include <vtextension/wasm/host/DescriptorRights.hpp>
#include <vtextension/wasm/host/HostBootstrap.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/host/macos/SandboxProfile.hpp>
#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/SharedObjectName.hpp>

using vtextension::wasm::host::ConfinementFailure;
using vtextension::wasm::host::confinementFailureText;
using vtextension::wasm::host::DescriptorRight;
using vtextension::wasm::host::DescriptorRole;
using vtextension::wasm::host::descriptorRoleRights;
using vtextension::wasm::host::GuestCode;
using vtextension::wasm::host::makeSystemSandbox;
using vtextension::wasm::host::NullSandbox;
using vtextension::wasm::host::PlatformSandbox;
using vtextension::wasm::host::profileParameterList;
using vtextension::wasm::host::ProfileParameters;
using vtextension::wasm::host::rightsOf;
using vtextension::wasm::host::SandboxDescriptors;
using vtextension::wasm::host::SandboxProfile;
using vtextension::wasm::host::ThisPlatformSandbox;

namespace proto = vtextension::wasm::remote::proto;

TEST_CASE("Sandbox.nullSandboxTakesBothStepsUnsandboxed", "[vtextension][host]")
{
    auto sandbox = NullSandbox {};
    CHECK(sandbox.restrictBeforeThreads().has_value());
    CHECK(sandbox.lockDown().has_value());
    CHECK(sandbox.lockdownState() == proto::LockdownState::Unsandboxed);
}

TEST_CASE("Sandbox.systemWithoutOneRunsUnsandboxed", "[vtextension][host]")
{
    // A system without a sandbox makes the null one, which confines nothing. So does Windows' for a process
    // its parent started in no AppContainer, as this test process is: it verifies, finds none, and confines
    // nothing. Where the sandbox confines on its own, it would confine this very process: the probe proves it
    // in a child of its own (SandboxProbe_test.cpp).
    if constexpr (ThisPlatformSandbox != PlatformSandbox::None
                  && ThisPlatformSandbox != PlatformSandbox::Windows)
        SKIP("this system has a sandbox");
    auto const sandbox = makeSystemSandbox(SandboxDescriptors { .control = {} }, GuestCode::Interpreted);
    REQUIRE(sandbox != nullptr);
    CHECK(sandbox->restrictBeforeThreads().has_value());
    CHECK(sandbox->lockDown().has_value());
    CHECK(sandbox->lockdownState() == proto::LockdownState::Unsandboxed);
}

TEST_CASE("Sandbox.failureSaysTheStepAndWhatTheSystemSaid", "[vtextension][host]")
{
    // The error code is read, not printed as a bare number: the line carries the system's word on it.
    auto const failure = ConfinementFailure { .step = proto::LockdownStep::SystemCalls, .code = 1 };
    auto const text = confinementFailureText(failure);
    INFO(text);
    CHECK(text
          == std::string { proto::lockdownStepText(proto::LockdownStep::SystemCalls) } + " ("
                 + proto::systemErrorText(1) + ")");
    CHECK_FALSE(proto::systemErrorText(1).empty());
    // As the parent phrases what a Hello states: the same words on both ends.
    CHECK(text == proto::lockdownFailureText({ .step = proto::LockdownStep::SystemCalls, .code = 1 }));
}

TEST_CASE("Sandbox.failureCodeIsReadAsItsStepSays", "[vtextension][host]")
{
    // A system's step states what the system said, and nothing for 0; priming states what the extension
    // runtime said; a value naming nothing leaves the step's text alone.
    CHECK(proto::lockdownFailureText({ .step = proto::LockdownStep::Job, .code = 0 })
          == proto::lockdownStepText(proto::LockdownStep::Job));
    auto const primed = proto::lockdownFailureText(
        { .step = proto::LockdownStep::Priming,
          .code = static_cast<uint32_t>(vtextension::ExtensionError::RuntimeUnavailable) });
    CHECK(primed
          == std::string { proto::lockdownStepText(proto::LockdownStep::Priming) }
                 + " (the extension runtime is unavailable)");
    CHECK(proto::lockdownFailureText({ .step = proto::LockdownStep::Priming, .code = 255 })
          == proto::lockdownStepText(proto::LockdownStep::Priming));
}

namespace
{
using vtextension::wasm::host::ContainerApi;
using vtextension::wasm::host::JobMembership;
using vtextension::wasm::host::TokenContainer;

/// What a fake container API was asked, and what it answers.
struct ContainerAnswers
{
    std::expected<TokenContainer, int> token = TokenContainer::AppContainer; ///< The token's answer.
    std::vector<std::expected<void, int>> mitigations = { {}, {} };          ///< Each mitigation's answer.
    std::expected<JobMembership, int> job = JobMembership::Inside;           ///< The job's answer.
    std::size_t mitigationsAsked = 0;                                        ///< How many were set.
    std::size_t jobAsked = 0; ///< How often the job was asked of.
};

/// @return An API answering as @p answers says, and counting into it.
[[nodiscard]] ContainerApi fakeContainerApi(std::shared_ptr<ContainerAnswers> const& answers)
{
    auto api = ContainerApi { .tokenContainer = [answers] { return answers->token; },
                              .mitigations = {},
                              .jobMembership =
                                  [answers] {
                                      ++answers->jobAsked;
                                      return answers->job;
                                  } };
    for (auto const index: std::views::iota(std::size_t { 0 }, answers->mitigations.size()))
        api.mitigations.emplace_back([answers, index] {
            ++answers->mitigationsAsked;
            return answers->mitigations.at(index);
        });
    return api;
}

/// @return What a container sandbox over @p answers makes of its lockdown, and the state it states then.
[[nodiscard]] std::pair<std::expected<void, ConfinementFailure>, proto::LockdownState> containerLockdown(
    std::shared_ptr<ContainerAnswers> const& answers)
{
    auto const sandbox = vtextension::wasm::host::makeContainerSandbox(fakeContainerApi(answers));
    auto const restricted = sandbox->restrictBeforeThreads();
    REQUIRE(restricted.has_value());
    auto const lockedDown = sandbox->lockDown();
    return { lockedDown, sandbox->lockdownState() };
}
} // namespace

TEST_CASE("Sandbox.containerDecidesUnsandboxedFailedOrLockedDown", "[vtextension][host]")
{
    // Windows' sandbox, over fake answers: wherever the host is built, as the decision is free of Windows.
    constexpr auto Refused = static_cast<int>(0x8007'0005U); // E_ACCESSDENIED
    auto const answers = std::make_shared<ContainerAnswers>();

    SECTION("in a container, every mitigation set, in a job: locked down")
    {
        auto const [lockedDown, state] = containerLockdown(answers);
        CHECK(lockedDown.has_value());
        CHECK(state == proto::LockdownState::LockedDown);
        CHECK(answers->mitigationsAsked == 2);
        CHECK(answers->jobAsked == 1);
    }
    SECTION("in no container: unsandboxed, and nothing else asked")
    {
        answers->token = TokenContainer::None;
        auto const [lockedDown, state] = containerLockdown(answers);
        CHECK(lockedDown.has_value());
        CHECK(state == proto::LockdownState::Unsandboxed);
        CHECK(answers->mitigationsAsked == 0);
        CHECK(answers->jobAsked == 0);
    }
    SECTION("a token that cannot be asked: the container fails")
    {
        answers->token = std::unexpected(Refused);
        auto const [lockedDown, state] = containerLockdown(answers);
        REQUIRE_FALSE(lockedDown.has_value());
        CHECK(lockedDown.error()
              == ConfinementFailure { .step = proto::LockdownStep::Container, .code = Refused });
        CHECK(state == proto::LockdownState::Unsandboxed);
    }
    SECTION("a refused mitigation fails, and every other step still runs")
    {
        answers->mitigations.front() = std::unexpected(Refused);
        answers->job = JobMembership::Outside;
        auto const [lockedDown, state] = containerLockdown(answers);
        REQUIRE_FALSE(lockedDown.has_value());
        // The first that failed is stated.
        CHECK(lockedDown.error()
              == ConfinementFailure { .step = proto::LockdownStep::Mitigations, .code = Refused });
        CHECK(answers->mitigationsAsked == 2);
        CHECK(answers->jobAsked == 1);
        CHECK(state != proto::LockdownState::LockedDown);
    }
    SECTION("in no job: the job fails, as a step of its own")
    {
        answers->job = JobMembership::Outside;
        auto const [lockedDown, state] = containerLockdown(answers);
        REQUIRE_FALSE(lockedDown.has_value());
        CHECK(lockedDown.error()
              == ConfinementFailure { .step = proto::LockdownStep::Job,
                                      .code = vtextension::wasm::host::NotInJobCode });
        CHECK(state != proto::LockdownState::LockedDown);
    }
    SECTION("a job that cannot be asked of: the job fails with what the system said")
    {
        answers->job = std::unexpected(Refused);
        auto const [lockedDown, state] = containerLockdown(answers);
        REQUIRE_FALSE(lockedDown.has_value());
        CHECK(lockedDown.error() == ConfinementFailure { .step = proto::LockdownStep::Job, .code = Refused });
    }
}

TEST_CASE("Sandbox.rightsTableCoversEveryRole", "[vtextension][host]")
{
    // One row per role, at its index, up to the last; each names what its role is used for, once each.
    auto const rows = descriptorRoleRights();
    REQUIRE(rows.size() == std::to_underlying(DescriptorRole::StopEvent) + std::size_t { 1 });
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
    {
        auto const& row = rows[index];
        INFO("role " << index);
        auto const roleIndex = std::size_t { std::to_underlying(row.role) };
        CHECK(roleIndex == index);
        CHECK_FALSE(row.rights.empty());
        CHECK(std::set<DescriptorRight>(row.rights.begin(), row.rights.end()).size() == row.rights.size());
        CHECK(std::ranges::equal(rightsOf(row.role), row.rights));
    }

    // What the host does with each after the lockdown: it talks over its channels and waits on its wake end,
    // and never writes to the page nor reads its standard error. The engine's device takes what a guest's
    // stdio does: writes, seeks, and its flags.
    auto const has = [](DescriptorRole role, DescriptorRight right) {
        return std::ranges::find(rightsOf(role), right) != rightsOf(role).end();
    };
    for (auto const role: { DescriptorRole::Control, DescriptorRole::Lane })
        for (auto const right: { DescriptorRight::Receive, DescriptorRight::Send, DescriptorRight::Event })
            CHECK(has(role, right));
    CHECK(has(DescriptorRole::Wake, DescriptorRight::Event));
    CHECK_FALSE(has(DescriptorRole::Page, DescriptorRight::Write));
    CHECK_FALSE(has(DescriptorRole::StandardError, DescriptorRight::Read));
    for (auto const right: { DescriptorRight::Write, DescriptorRight::Seek, DescriptorRight::GetStatusFlags })
        CHECK(has(DescriptorRole::EngineDevice, right));
    // Standard input and output are only asked what they are; the stop pipe is waited on at one end and
    // written to at the other.
    CHECK(std::ranges::equal(rightsOf(DescriptorRole::IdleStream), std::array { DescriptorRight::Fstat }));
    CHECK(has(DescriptorRole::StopReceiver, DescriptorRight::Event));
    CHECK_FALSE(has(DescriptorRole::StopReceiver, DescriptorRight::Write));
    CHECK(std::ranges::equal(rightsOf(DescriptorRole::StopSender), std::array { DescriptorRight::Write }));
    // The stop event is both ends at once: what each of them may, and no more.
    CHECK(std::ranges::equal(
        rightsOf(DescriptorRole::StopEvent),
        std::array { DescriptorRight::Read, DescriptorRight::Write, DescriptorRight::Event }));
}

namespace
{

/// One top-level form of a profile: `(<head> <body>)`.
struct ProfileForm
{
    std::string head; ///< The word the form starts with, after any space.
    std::string body; ///< What follows the head, up to the form's closing parenthesis, comments removed.
};

/// @return The top-level forms of @p profile, in order: `;` comments removed, except inside a string
///         literal; nothing if it is not one form after another -- unbalanced parentheses, an unterminated
///         string, or text outside every form.
[[nodiscard]] std::optional<std::vector<ProfileForm>> topLevelFormsOf(std::string_view profile)
{
    /// Where the reading is within the text.
    enum class Lexeme : uint8_t
    {
        Code,    ///< Outside any string or comment.
        String,  ///< Inside a string literal.
        Escaped, ///< Right after a backslash inside a string literal.
        Comment, ///< From a `;` outside a string to the end of its line.
    };
    constexpr auto Space = std::string_view { " \t\n" };
    constexpr auto Delimiters = std::string_view { " \t\n()\";" };
    auto forms = std::vector<ProfileForm> {};
    auto depth = std::size_t { 0 };
    auto lexeme = Lexeme::Code;
    auto text = std::string {}; // The current form's text after its opening parenthesis.
    for (auto const c: profile)
    {
        switch (lexeme)
        {
            case Lexeme::Comment:
                if (c == '\n')
                    lexeme = Lexeme::Code;
                continue;
            case Lexeme::Escaped:
                lexeme = Lexeme::String;
                text += c;
                continue;
            case Lexeme::String:
                if (c == '\\')
                    lexeme = Lexeme::Escaped;
                else if (c == '"')
                    lexeme = Lexeme::Code;
                text += c;
                continue;
            case Lexeme::Code: break;
        }
        if (c == ';')
        {
            lexeme = Lexeme::Comment;
            continue;
        }
        if (depth == 0)
        {
            if (c == '(')
                ++depth;
            else if (!Space.contains(c))
                return std::nullopt;
            continue;
        }
        if (c == ')' && --depth == 0)
        {
            auto const view = std::string_view { text };
            auto const head = std::min(view.find_first_not_of(Space), view.size());
            auto const end = std::min(view.find_first_of(Delimiters, head), view.size());
            forms.push_back(ProfileForm { .head = std::string { view.substr(head, end - head) },
                                          .body = std::string { view.substr(end) } });
            text.clear();
            continue;
        }
        if (c == '(')
            ++depth;
        else if (c == '"')
            lexeme = Lexeme::String;
        text += c;
    }
    if (depth != 0 || lexeme == Lexeme::String || lexeme == Lexeme::Escaped)
        return std::nullopt;
    return forms;
}

/// @return The operation names of every form of @p forms whose head is @p action, in order, once per form
///         that names them: the words after the head, up to the form's first filter or its end.
[[nodiscard]] std::vector<std::string> operationsOf(std::span<ProfileForm const> forms,
                                                    std::string_view action)
{
    constexpr auto Space = std::string_view { " \t\n" };
    constexpr auto Delimiters = std::string_view { " \t\n()" };
    auto operations = std::vector<std::string> {};
    for (auto const& form: forms | std::views::filter([&](auto const& form) { return form.head == action; }))
    {
        auto rest = std::string_view { form.body };
        while (true)
        {
            auto const word = rest.find_first_not_of(Space);
            if (word == std::string_view::npos || rest[word] == '(' || rest[word] == ')')
                break;
            rest.remove_prefix(word);
            auto const end = std::min(rest.find_first_of(Delimiters), rest.size());
            operations.emplace_back(rest.substr(0, end));
            rest.remove_prefix(end);
        }
    }
    return operations;
}

/// @return The operation names of every `(<action> …)` form of @p profile (operationsOf()); nothing if the
///         profile does not parse.
[[nodiscard]] std::optional<std::vector<std::string>> operationsOf(std::string_view profile,
                                                                   std::string_view action)
{
    auto const forms = topLevelFormsOf(profile);
    if (!forms)
        return std::nullopt;
    return operationsOf(*forms, action);
}

/// Every head a top-level form of the profile may have: what deny-by-default and its exceptions take. Any
/// other -- `import`, `define`, `if` -- would add rules this test cannot read.
constexpr auto AllowedFormHeads = std::array<std::string_view, 3> { "version", "deny", "allow" };

/// Every operation the profile allows, each in exactly one form: what the locked-down host still does that
/// Seatbelt mediates (SandboxProfile.hpp says why each).
constexpr auto AllowedOperations = std::array<std::string_view, 5> {
    "ipc-posix-shm-read-data",
    "ipc-posix-shm-read-metadata",
    "ipc-posix-shm-write-data",
    "signal",
    "sysctl-read",
};

} // namespace

TEST_CASE("Sandbox.profileHasNoNetworkOrMachLookup", "[vtextension][host]")
{
    // macOS's profile, read on every system. It denies by default, and allows exactly the operations of
    // AllowedOperations, each named in full in one form: no wildcard can widen a rule, and an operation the
    // table does not hold -- network, Mach lookup, files, processes, IOKit -- cannot be allowed by accident.
    auto const forms = topLevelFormsOf(SandboxProfile);
    REQUIRE(forms.has_value());
    for (auto const& form: *forms)
    {
        INFO("form head: " << form.head);
        CHECK(std::ranges::contains(AllowedFormHeads, std::string_view { form.head }));
    }
    CHECK(SandboxProfile.starts_with("(version 1)\n(deny default)\n"));
    CHECK(std::ranges::equal(operationsOf(*forms, "deny"), std::array { std::string_view { "default" } }));
    CHECK_FALSE(SandboxProfile.contains('*'));

    auto allowed = operationsOf(*forms, "allow");
    std::ranges::sort(allowed);
    auto expected = AllowedOperations;
    std::ranges::sort(expected);
    auto listed = std::string {};
    for (auto const& operation: allowed)
        listed += operation + ' ';
    INFO("allowed: " << listed);
    CHECK(std::ranges::equal(allowed, expected));

    // Each rule's filter: shared memory only by the prefix Contour names its objects with, read from a
    // parameter; signals only to itself; sysctls only the page size and the CPU feature names.
    CHECK(SandboxProfile.contains("(ipc-posix-name-prefix (param \"SHM_PREFIX\"))"));
    CHECK(SandboxProfile.contains("(allow signal (target self))"));
    CHECK(SandboxProfile.contains("(sysctl-name \"hw.pagesize_compat\")"));
    CHECK(SandboxProfile.contains("(sysctl-name-prefix \"hw.optional.\")"));
}

TEST_CASE("Sandbox.profileParserSeesWhatAWildcardWouldHide", "[vtextension][host]")
{
    // The reading above, on profiles it must refuse: a second form, a wildcard, an operation not in the
    // table, a head spaced from its parenthesis, a comment sign inside a string, a head not in the table.
    using Operations = std::optional<std::vector<std::string>>;
    CHECK(operationsOf("(allow file* (subpath \"/\"))\n(allow mach-lookup)", "allow")
          == Operations { { "file*", "mach-lookup" } });
    CHECK(operationsOf("(allow signal)\n(allow signal (target self))", "allow")
          == Operations { { "signal", "signal" } });
    CHECK(operationsOf("(allowed x)", "allow") == Operations { std::vector<std::string> {} });
    CHECK(operationsOf("( allow\tfile-read*)", "allow") == Operations { { "file-read*" } });
    CHECK(operationsOf("(allow a (literal \"x;y\"))\n(allow b)", "allow") == Operations { { "a", "b" } });
    CHECK(operationsOf("(allow a) ; (allow b)\n", "allow") == Operations { { "a" } });
    auto const imported = topLevelFormsOf("(version 1)\n(import \"system.sb\")");
    REQUIRE(imported.has_value());
    CHECK_FALSE(std::ranges::all_of(*imported, [](auto const& form) {
        return std::ranges::contains(AllowedFormHeads, std::string_view { form.head });
    }));
    // Not one form after another: nothing is read at all.
    CHECK_FALSE(topLevelFormsOf("(allow a").has_value());
    CHECK_FALSE(topLevelFormsOf("(allow a (literal \"x)").has_value());
    CHECK_FALSE(topLevelFormsOf("allow a").has_value());
}

TEST_CASE("Sandbox.profileParametersAreWhatTheProfileReads", "[vtextension][host]")
{
    // Every parameter the profile reads is passed, and the shared memory prefix is the one every object's
    // name starts with.
    for (auto const& parameter: ProfileParameters)
    {
        INFO(parameter.name);
        CHECK(SandboxProfile.contains(std::string { "(param \"" } + std::string { parameter.name } + "\")"));
    }
    auto const prefix = std::ranges::find(ProfileParameters,
                                          std::string_view { "SHM_PREFIX" },
                                          &vtextension::wasm::host::ProfileParameter::name);
    REQUIRE(prefix != ProfileParameters.end());
    CHECK(vtextension::wasm::remote::transport::sharedObjectName(42, 1).starts_with(prefix->value));

    // As the C API takes them: name, value, ..., then a null pointer.
    constexpr auto List = profileParameterList();
    REQUIRE(List.size() == (2 * ProfileParameters.size()) + 1);
    for (auto const index: std::views::iota(std::size_t { 0 }, ProfileParameters.size()))
    {
        CHECK(std::string_view { List[2 * index] } == ProfileParameters[index].name);
        CHECK(std::string_view { List[(2 * index) + 1] } == ProfileParameters[index].value);
    }
    CHECK(List.back() == nullptr);
}
