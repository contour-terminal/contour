// SPDX-License-Identifier: Apache-2.0
//
// The file system restriction against a fake kernel: which ABI it uses, what it hands the kernel, and which
// of the kernel's answers are failures.
#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <vtextension/wasm/host/linux/Landlock.hpp>

using namespace vtextension::wasm::host;
using vtextension::wasm::remote::proto::LockdownStep;

namespace
{

/// What the fake system calls answer: an ABI or a descriptor, or `-errno`.
struct Answers
{
    long version = 0;    ///< The version query.
    long created = 0;    ///< Creating the ruleset.
    long restricted = 0; ///< Restricting.
};

/// The kernel the fake system calls answer as, and what they were asked. One case at a time.
struct FakeKernel
{
    Answers answers;                         ///< What they answer.
    std::optional<LandlockRulesetAttr> attr; ///< What the ruleset was created with.
    std::vector<PathBeneathRule> rules;      ///< What was added.
    std::optional<uint32_t> restrictFlags;   ///< What restricting was passed.
    std::vector<int> closed;                 ///< What was closed.
};

/// @return The fake kernel the fake system calls answer as: they are plain functions, which capture nothing.
[[nodiscard]] FakeKernel& kernel()
{
    static auto instance = FakeKernel {};
    return instance;
}

constexpr auto Ruleset = 42;

[[nodiscard]] LandlockSystem fakeSystem(Answers answers)
{
    kernel() = FakeKernel { .answers = answers, .attr = {}, .rules = {}, .restrictFlags = {}, .closed = {} };
    return LandlockSystem {
        .createRuleset =
            [](LandlockRulesetAttr const* attr, std::size_t size, uint32_t flags) noexcept -> long {
            if (attr == nullptr)
                return size == 0 && flags == 1 ? kernel().answers.version : -EINVAL;
            kernel().attr = *attr;
            return kernel().answers.created;
        },
        .addPathBeneathRule = [](int ruleset, PathBeneathRule rule) noexcept -> long {
            if (ruleset != Ruleset)
                return -EBADF;
            kernel().rules.push_back(rule);
            return 0;
        },
        .restrictSelf = [](int ruleset, uint32_t flags) noexcept -> long {
            if (ruleset != Ruleset)
                return -EBADF;
            kernel().restrictFlags = flags;
            return kernel().answers.restricted;
        },
        .closeRuleset = [](int ruleset) noexcept { kernel().closed.push_back(ruleset); },
    };
}

[[nodiscard]] LandlockAbiRow const& abiRow(int abi)
{
    return landlockAbis()[static_cast<std::size_t>(abi - 1)];
}

} // namespace

TEST_CASE("Landlock.abiTableIsMonotonic", "[vtextension][sandbox]")
{
    auto const abis = landlockAbis();
    REQUIRE_FALSE(abis.empty());
    CHECK(abis.front().abi == 1);
    for (auto const index: std::views::iota(std::size_t { 1 }, abis.size()))
    {
        auto const& before = abis[index - 1];
        auto const& row = abis[index];
        INFO("ABI " << row.abi);
        CHECK(row.abi == before.abi + 1);
        // Each ABI knows all its predecessor knew.
        CHECK((row.fsRights & before.fsRights) == before.fsRights);
        CHECK((row.netRights & before.netRights) == before.netRights);
        CHECK((row.scopes & before.scopes) == before.scopes);
        CHECK((row.restrictFlags & before.restrictFlags) == before.restrictFlags);
    }
    // Network rights from 4, scopes from 6, TSYNC from 8.
    CHECK(abiRow(3).netRights == 0);
    CHECK(abiRow(4).netRights != 0);
    CHECK(abiRow(5).scopes == 0);
    CHECK(abiRow(6).scopes != 0);
    CHECK(abiRow(7).restrictFlags == 0);
    CHECK(abiRow(8).restrictFlags != 0);
}

TEST_CASE("Landlock.usesTheBestAbiTheKernelHas", "[vtextension][sandbox]")
{
    auto const directory =
        PathBeneathRule { .parent = 5, .allowedAccess = LandlockReadFile | LandlockTruncate };

    SECTION("ABI 3: no network rights, no scopes, no TSYNC")
    {
        auto const restricted = restrictFileSystem(std::span { &directory, 1 },
                                                   fakeSystem(Answers { .version = 3, .created = Ruleset }));
        REQUIRE(restricted == LandlockOutcome::Restricted);
        REQUIRE(kernel().attr.has_value());
        CHECK(kernel().attr->handledAccessFs == abiRow(3).fsRights);
        CHECK(kernel().attr->handledAccessNet == 0);
        CHECK(kernel().attr->scoped == 0);
        CHECK(kernel().restrictFlags == 0U);
        REQUIRE(kernel().rules.size() == 1);
        CHECK(kernel().rules.front().parent == 5);
        CHECK(kernel().rules.front().allowedAccess == (LandlockReadFile | LandlockTruncate));
    }
    SECTION("ABI 2 grants no right it does not handle")
    {
        REQUIRE(restrictFileSystem(std::span { &directory, 1 },
                                   fakeSystem(Answers { .version = 2, .created = Ruleset }))
                == LandlockOutcome::Restricted);
        REQUIRE(kernel().rules.size() == 1);
        CHECK(kernel().rules.front().allowedAccess == LandlockReadFile);
    }
    SECTION("ABI 8 and later: TSYNC; a kernel newer than the table gets the table's best")
    {
        for (auto const version: { 8L, 10L, 99L })
        {
            REQUIRE(restrictFileSystem({}, fakeSystem(Answers { .version = version, .created = Ruleset }))
                    == LandlockOutcome::Restricted);
            auto const& best = version > landlockAbis().back().abi ? landlockAbis().back()
                                                                   : abiRow(static_cast<int>(version));
            REQUIRE(kernel().attr.has_value());
            CHECK(kernel().attr->handledAccessFs == best.fsRights);
            CHECK(kernel().attr->handledAccessNet == best.netRights);
            CHECK(kernel().attr->scoped == best.scopes);
            CHECK(kernel().restrictFlags == best.restrictFlags);
            CHECK((best.restrictFlags & 0b1000U) != 0);
            CHECK(kernel().closed == std::vector<int> { Ruleset });
        }
    }
}

TEST_CASE("Landlock.unavailableIsNotAFailure", "[vtextension][sandbox]")
{
    for (auto const error: { ENOSYS, EOPNOTSUPP })
    {
        INFO(error);
        // Asked of the version, or of the ruleset.
        CHECK(restrictFileSystem({}, fakeSystem(Answers { .version = -error }))
              == LandlockOutcome::Unavailable);
        CHECK(kernel().closed.empty());
        CHECK(restrictFileSystem({}, fakeSystem(Answers { .version = 6, .created = -error }))
              == LandlockOutcome::Unavailable);
        CHECK_FALSE(kernel().restrictFlags.has_value());
    }
}

TEST_CASE("Landlock.otherErrorsAreFailures", "[vtextension][sandbox]")
{
    // Restricting without no_new_privs.
    auto const restricted = restrictFileSystem(
        {}, fakeSystem(Answers { .version = 10, .created = Ruleset, .restricted = -EPERM }));
    REQUIRE_FALSE(restricted.has_value());
    CHECK(restricted.error() == ConfinementFailure { .step = LockdownStep::FileSystem, .code = EPERM });
    CHECK(kernel().closed == std::vector<int> { Ruleset });

    // Anything else the version query or the ruleset's creation says.
    CHECK(restrictFileSystem({}, fakeSystem(Answers { .version = -EFAULT })).error()
          == ConfinementFailure { .step = LockdownStep::FileSystem, .code = EFAULT });
    CHECK(restrictFileSystem({}, fakeSystem(Answers { .version = 4, .created = -ENOMEM })).error()
          == ConfinementFailure { .step = LockdownStep::FileSystem, .code = ENOMEM });
}
