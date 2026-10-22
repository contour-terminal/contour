// SPDX-License-Identifier: Apache-2.0
//
// The statusline world's types as the Canonical ABI codec sees them: the rows that describe its
// records, enums and flags, and the types that exist only on the wire (`command-output`,
// `command-error`).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/IHostServices.hpp>

namespace vtextension::abi
{

/// The world's `command-output`: what a command that ran to completion left behind.
struct CommandOutput
{
    int32_t exitCode = 0;    ///< The command's exit code.
    ByteList standardOutput; ///< What it wrote to its standard output.
    ByteList standardError;  ///< What it wrote to its standard error.

    [[nodiscard]] bool operator==(CommandOutput const&) const = default;
};

/// The world's `command-error`: why a command did not produce output.
enum class CommandError : uint8_t
{
    NotDeclared, ///< The manifest does not declare this command with these arguments.
    Pending,     ///< The user has not answered yet.
    Denied,      ///< The user refused.
    Failed,      ///< The command could not be started or did not exit normally.
    TimedOut,    ///< The command ran longer than the host allows.
};

/// What `run-command` answers.
using CommandResult = std::expected<CommandOutput, CommandError>;

/// One row of the table that turns a host error into the `command-error` the guest sees.
struct CommandErrorMapping
{
    ExtensionError error;      ///< The host's error.
    CommandError commandError; ///< What the guest is told.
};

/// The host errors a guest is told about specifically; every other one is `failed`.
inline constexpr auto CommandErrorMappings = std::array {
    CommandErrorMapping { .error = ExtensionError::CommandNotDeclared,
                          .commandError = CommandError::NotDeclared },
    CommandErrorMapping { .error = ExtensionError::ArgumentRejected,
                          .commandError = CommandError::NotDeclared },
    CommandErrorMapping { .error = ExtensionError::PermissionPending, .commandError = CommandError::Pending },
    CommandErrorMapping { .error = ExtensionError::PermissionDenied, .commandError = CommandError::Denied },
    CommandErrorMapping { .error = ExtensionError::DeadlineExceeded, .commandError = CommandError::TimedOut },
};

/// @param error Why the host did not run a command, or why it did not finish.
/// @return The `command-error` the guest is told.
[[nodiscard]] constexpr CommandError commandErrorFor(ExtensionError error) noexcept
{
    auto const row = std::ranges::find(CommandErrorMappings, error, &CommandErrorMapping::error);
    return row != CommandErrorMappings.end() ? row->commandError : CommandError::Failed;
}

// clang-format off
template <> struct RecordFields<Progress> { static constexpr auto Members = std::tuple { &Progress::state, &Progress::percent }; };
template <> struct RecordFields<Tab> { static constexpr auto Members = std::tuple { &Tab::title, &Tab::activity }; };
template <> struct RecordFields<SurfaceSize> { static constexpr auto Members = std::tuple { &SurfaceSize::cols, &SurfaceSize::rows }; };
template <> struct RecordFields<TitleChanged> { static constexpr auto Members = std::tuple { &TitleChanged::title }; };
template <> struct RecordFields<CwdChanged> { static constexpr auto Members = std::tuple { &CwdChanged::cwd }; };
template <> struct RecordFields<ForegroundProcessChanged> { static constexpr auto Members = std::tuple { &ForegroundProcessChanged::name }; };
template <> struct RecordFields<InputModeChanged> { static constexpr auto Members = std::tuple { &InputModeChanged::mode }; };
template <> struct RecordFields<TabsChanged> { static constexpr auto Members = std::tuple { &TabsChanged::tabs }; };
template <> struct RecordFields<UserVarChanged> { static constexpr auto Members = std::tuple { &UserVarChanged::name, &UserVarChanged::value }; };
template <> struct RecordFields<ProgressChanged> { static constexpr auto Members = std::tuple { &ProgressChanged::progress }; };
template <> struct RecordFields<FontMetrics> { static constexpr auto Members = std::tuple { &FontMetrics::pointSize, &FontMetrics::cellWidth, &FontMetrics::cellHeight }; };
template <> struct RecordFields<FontChanged> { static constexpr auto Members = std::tuple { &FontChanged::metrics }; };
template <> struct RecordFields<FocusChanged> { static constexpr auto Members = std::tuple { &FocusChanged::state }; };
template <> struct RecordFields<Resized> { static constexpr auto Members = std::tuple { &Resized::size }; };
template <> struct RecordFields<TimerFired> { static constexpr auto Members = std::tuple {}; };
template <> struct RecordFields<ReplyBytes> { static constexpr auto Members = std::tuple { &ReplyBytes::bytes }; };
template <> struct RecordFields<CommandOutput> { static constexpr auto Members = std::tuple { &CommandOutput::exitCode, &CommandOutput::standardOutput, &CommandOutput::standardError }; };
template <> struct RecordFields<StateSnapshot> { static constexpr auto Members = std::tuple {
    &StateSnapshot::title, &StateSnapshot::cwd, &StateSnapshot::foregroundProcess, &StateSnapshot::inputMode,
    &StateSnapshot::tabs, &StateSnapshot::userVars, &StateSnapshot::progress, &StateSnapshot::focus,
    &StateSnapshot::size, &StateSnapshot::now, &StateSnapshot::utcOffset, &StateSnapshot::font }; };

template <> inline constexpr uint32_t EnumCaseCount<RenderRequest> = 2;
template <> inline constexpr uint32_t EnumCaseCount<InputMode> = 6;
template <> inline constexpr uint32_t EnumCaseCount<ProgressState> = 5;
template <> inline constexpr uint32_t EnumCaseCount<LogLevel> = 5;
template <> inline constexpr uint32_t EnumCaseCount<CommandError> = 5;
template <> inline constexpr uint32_t EnumCaseCount<TabActivity> = 2;
template <> inline constexpr uint32_t EnumCaseCount<FocusState> = 2;
template <> inline constexpr bool IsWireBool<TabActivity> = true;
template <> inline constexpr bool IsWireBool<FocusState> = true;

template <> inline constexpr uint32_t FlagsMemberCount<EventKind> = 9;
// clang-format on

/// Whether a value of a type holds an f32 anywhere: in a field of a record, an element of a list, or a case
/// of a variant. @see the assertions below, which is what Codec<float>'s flat form relies on.
template <typename T>
struct HoldsFloatTrait: std::bool_constant<std::is_same_v<T, float>>
{
};

/// @copydoc HoldsFloatTrait
template <typename T>
inline constexpr bool HoldsFloat = HoldsFloatTrait<T>::value;

/// @return Whether a field of the record @p T holds an f32.
template <typename T>
[[nodiscard]] consteval bool recordHoldsFloat()
{
    return std::apply(
        [](auto... members) {
            return (false || ... || HoldsFloat<std::remove_cvref_t<decltype(std::declval<T&>().*members)>>);
        },
        RecordFields<T>::Members);
}

template <typename T>
    requires requires { RecordFields<T>::Members; }
struct HoldsFloatTrait<T>: std::bool_constant<recordHoldsFloat<T>()>
{
};

template <typename T>
struct HoldsFloatTrait<std::vector<T>>: HoldsFloatTrait<T>
{
};

template <typename... Ts>
struct HoldsFloatTrait<std::variant<Ts...>>: std::bool_constant<(false || ... || HoldsFloat<Ts>)>
{
};

template <typename T, typename E>
struct HoldsFloatTrait<std::expected<T, E>>: std::bool_constant<HoldsFloat<T> || HoldsFloat<E>>
{
};

/// @return How many of a variant's cases hold an f32.
template <typename... Ts>
[[nodiscard]] consteval std::size_t floatHoldingCases(std::variant<Ts...> const* /*variant*/) noexcept
{
    return (std::size_t { 0 } + ... + std::size_t { HoldsFloat<Ts> });
}

// Codec<float> lowers an f32 flat as the i32 of its bits. That is the Canonical ABI only where the f32 is
// joined with an i32 of another case at the same flat position; a bare f32 parameter or result would be an
// f32 core value. So: no flat-passed type of the world but `event` holds an f32 (`state` and `run-command`
// return through memory, where an f32 is just its four bytes) ...
static_assert(
    !HoldsFloat<std::vector<std::pair<std::string, std::string>>> /* load's config */
        && !HoldsFloat<std::string> && !HoldsFloat<LogLevel> && !HoldsFloat<EventKinds>
        && !HoldsFloat<RenderRequest> && !HoldsFloat<uint32_t>,
    "an f32 passed flat outside a variant needs an f32 flat type, which Codec<float> does not have");
// ... within `event` only `font-changed` does, and `user-var`, a tuple of two strings, fills every flat
// position font-changed's payload does with an i32 to join it with.
static_assert(floatHoldingCases(static_cast<Event const*>(nullptr)) == 1 && HoldsFloat<FontChanged>,
              "another case of event holding an f32 needs its flat join checked");
static_assert(!HoldsFloat<UserVarChanged>
                  && Codec<FontChanged>::FlatCount <= Codec<UserVarChanged>::FlatCount,
              "font-changed's f32 is no longer joined with an i32 of user-var");

} // namespace vtextension::abi
