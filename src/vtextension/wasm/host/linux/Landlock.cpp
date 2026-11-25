// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/linux/Landlock.hpp>

#include <sys/syscall.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <ranges>

#include <unistd.h>

#include <vtextension/wasm/host/linux/SystemCallRows.hpp>

namespace vtextension::wasm::host
{

namespace
{
    // File system rights, by the ABI that added them (LANDLOCK_ACCESS_FS_*).
    constexpr auto FirstFsRights = uint64_t { 0b1'1111'1111'1111 }; // EXECUTE … MAKE_SYM
    constexpr auto Refer = uint64_t { 0b10'0000'0000'0000 };
    constexpr auto IoctlDevice = uint64_t { 0b1000'0000'0000'0000 };
    constexpr auto ResolveUnix = uint64_t { 0b1'0000'0000'0000'0000 };
    // Network rights (LANDLOCK_ACCESS_NET_*).
    constexpr auto BindTcp = uint64_t { 0b0001 };
    constexpr auto ConnectTcp = uint64_t { 0b0010 };
    constexpr auto BindUdp = uint64_t { 0b0100 };
    constexpr auto ConnectSendUdp = uint64_t { 0b1000 };
    // Scopes (LANDLOCK_SCOPE_*).
    constexpr auto AbstractUnixSocket = uint64_t { 0b01 };
    constexpr auto Signal = uint64_t { 0b10 };
    // landlock_restrict_self() flags (LANDLOCK_RESTRICT_SELF_*).
    constexpr auto ThreadSync = uint32_t { 0b1000 };
    // landlock_create_ruleset()'s flag asking for the ABI version.
    constexpr auto RulesetVersion = uint32_t { 0b0001 };
    // landlock_add_rule()'s rule type of a directory and what lies beneath it.
    constexpr auto RulePathBeneath = 1;

    constexpr auto Abi2Fs = FirstFsRights | Refer;
    constexpr auto Abi3Fs = Abi2Fs | LandlockTruncate;
    constexpr auto Abi5Fs = Abi3Fs | IoctlDevice;
    constexpr auto Abi9Fs = Abi5Fs | ResolveUnix;
    constexpr auto TcpRights = BindTcp | ConnectTcp;
    constexpr auto AllNetRights = TcpRights | BindUdp | ConnectSendUdp;
    constexpr auto AllScopes = AbstractUnixSocket | Signal;

    // Each ABI as the kernel's Documentation/userspace-api/landlock.rst lists what it added.
    constexpr auto AbiTable = std::array {
        LandlockAbiRow {
            .abi = 1, .fsRights = FirstFsRights, .netRights = 0, .scopes = 0, .restrictFlags = 0 },
        LandlockAbiRow { .abi = 2, .fsRights = Abi2Fs, .netRights = 0, .scopes = 0, .restrictFlags = 0 },
        LandlockAbiRow { .abi = 3, .fsRights = Abi3Fs, .netRights = 0, .scopes = 0, .restrictFlags = 0 },
        LandlockAbiRow {
            .abi = 4, .fsRights = Abi3Fs, .netRights = TcpRights, .scopes = 0, .restrictFlags = 0 },
        LandlockAbiRow {
            .abi = 5, .fsRights = Abi5Fs, .netRights = TcpRights, .scopes = 0, .restrictFlags = 0 },
        LandlockAbiRow {
            .abi = 6, .fsRights = Abi5Fs, .netRights = TcpRights, .scopes = AllScopes, .restrictFlags = 0 },
        LandlockAbiRow {
            .abi = 7, .fsRights = Abi5Fs, .netRights = TcpRights, .scopes = AllScopes, .restrictFlags = 0 },
        LandlockAbiRow { .abi = 8,
                         .fsRights = Abi5Fs,
                         .netRights = TcpRights,
                         .scopes = AllScopes,
                         .restrictFlags = ThreadSync },
        LandlockAbiRow { .abi = 9,
                         .fsRights = Abi9Fs,
                         .netRights = TcpRights,
                         .scopes = AllScopes,
                         .restrictFlags = ThreadSync },
        LandlockAbiRow { .abi = 10,
                         .fsRights = Abi9Fs,
                         .netRights = AllNetRights,
                         .scopes = AllScopes,
                         .restrictFlags = ThreadSync },
    };

    /// What landlock_add_rule() is given for a LANDLOCK_RULE_PATH_BENEATH rule, laid out as the kernel's.
    struct [[gnu::packed]] PathBeneathAttr
    {
        uint64_t allowedAccess;
        int32_t parentFd;
    };

    /// @return @p result, a raw system call's, as the result or `-errno`.
    [[nodiscard]] long resultOrError(long result) noexcept
    {
        return result < 0 ? -errno : result;
    }

    /// @return The number of @p call here.
    [[nodiscard]] long numberOf(SystemCall call) noexcept
    {
        return systemCallNumber(call, ThisAuditArch).value_or(NoSystemCall);
    }

    [[nodiscard]] bool isUnavailable(long error) noexcept
    {
        return error == -ENOSYS || error == -EOPNOTSUPP;
    }

    [[nodiscard]] ConfinementFailure fileSystemFailure(long error) noexcept
    {
        return ConfinementFailure { .step = remote::proto::LockdownStep::FileSystem,
                                    .code = static_cast<int>(-error) };
    }
} // namespace

std::span<LandlockAbiRow const> landlockAbis() noexcept
{
    return AbiTable;
}

LandlockSystem systemLandlock() noexcept
{
    return LandlockSystem {
        .createRuleset =
            [](LandlockRulesetAttr const* attr, std::size_t size, uint32_t flags) noexcept {
                return resultOrError(
                    ::syscall(numberOf(SystemCall::LandlockCreateRuleset), attr, size, flags));
            },
        .addPathBeneathRule =
            [](int ruleset, PathBeneathRule rule) noexcept {
                auto const attr =
                    PathBeneathAttr { .allowedAccess = rule.allowedAccess, .parentFd = rule.parent };
                return resultOrError(
                    ::syscall(numberOf(SystemCall::LandlockAddRule), ruleset, RulePathBeneath, &attr, 0U));
            },
        .restrictSelf =
            [](int ruleset, uint32_t flags) noexcept {
                return resultOrError(::syscall(numberOf(SystemCall::LandlockRestrictSelf), ruleset, flags));
            },
        .closeRuleset = [](int ruleset) noexcept { ::close(ruleset); },
    };
}

std::expected<LandlockOutcome, ConfinementFailure> restrictFileSystem(
    std::span<PathBeneathRule const> allowed, LandlockSystem const& system)
{
    auto const version = system.createRuleset(nullptr, 0, RulesetVersion);
    if (isUnavailable(version))
        return LandlockOutcome::Unavailable;
    if (version < 0)
        return std::unexpected(fileSystemFailure(version));
    // The best ABI both know: the kernel's, or our newest if the kernel's is newer still.
    auto const newestFirst = AbiTable | std::views::reverse;
    auto const known = std::ranges::find_if(
        newestFirst, [version](LandlockAbiRow const& row) { return row.abi <= version; });
    if (known == newestFirst.end())
        return LandlockOutcome::Unavailable;
    auto const& abi = *known;

    auto const attr = LandlockRulesetAttr { .handledAccessFs = abi.fsRights,
                                            .handledAccessNet = abi.netRights,
                                            .scoped = abi.scopes };
    auto const ruleset = system.createRuleset(&attr, sizeof(attr), 0);
    if (isUnavailable(ruleset))
        return LandlockOutcome::Unavailable;
    if (ruleset < 0)
        return std::unexpected(fileSystemFailure(ruleset));
    auto const descriptor = static_cast<int>(ruleset);
    auto const restricted = [&]() -> std::expected<LandlockOutcome, ConfinementFailure> {
        for (auto const& rule: allowed)
        {
            auto const granted =
                PathBeneathRule { .parent = rule.parent, .allowedAccess = rule.allowedAccess & abi.fsRights };
            if (auto const added = system.addPathBeneathRule(descriptor, granted); added < 0)
                return std::unexpected(fileSystemFailure(added));
        }
        if (auto const enforced = system.restrictSelf(descriptor, abi.restrictFlags); enforced < 0)
            return std::unexpected(fileSystemFailure(enforced));
        return LandlockOutcome::Restricted;
    }();
    system.closeRuleset(descriptor);
    return restricted;
}

} // namespace vtextension::wasm::host
