// SPDX-License-Identifier: Apache-2.0
#include <core/Flags.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <string>

namespace
{
enum class Fruit : uint8_t
{
    Apple = 1 << 0,
    Banana = 1 << 1,
    Cherry = 1 << 2,
};

using Fruits = core::Flags<Fruit>;

constexpr auto AllFruits = Fruits { Fruit::Apple, Fruit::Banana, Fruit::Cherry };
} // namespace

template <>
struct std::formatter<Fruit>: std::formatter<std::string>
{
    auto format(Fruit fruit, auto& ctx) const
    {
        auto const* name = [&]() -> char const* {
            switch (fruit)
            {
                case Fruit::Apple: return "Apple";
                case Fruit::Banana: return "Banana";
                case Fruit::Cherry: return "Cherry";
            }
            return "";
        }();
        return std::formatter<std::string>::format(name, ctx);
    }
};

TEST_CASE("Flags: a default-constructed set holds nothing", "[base][flags]")
{
    auto const flags = Fruits {};
    CHECK(flags.none());
    CHECK(!flags.any());
    CHECK(!flags);
    CHECK(!static_cast<bool>(flags));
    CHECK(flags.value() == 0);
}

TEST_CASE("Flags: construction names the members", "[base][flags]")
{
    CHECK(Fruits { Fruit::Apple }.value() == 0b001);
    CHECK((Fruits { Fruit::Apple, Fruit::Cherry }.value() == 0b101));
    CHECK(AllFruits.value() == 0b111);
}

TEST_CASE("Flags: enable and disable are each other's inverse", "[base][flags]")
{
    auto flags = Fruits {};

    flags.enable(Fruit::Apple);
    CHECK(flags.test(Fruit::Apple));
    CHECK(!flags.test(Fruit::Banana));

    flags.enable(Fruits { Fruit::Banana, Fruit::Cherry });
    CHECK(flags == AllFruits);

    flags.disable(Fruit::Banana);
    CHECK(flags == (Fruits { Fruit::Apple, Fruit::Cherry }));

    flags.disable(Fruits { Fruit::Apple, Fruit::Cherry });
    CHECK(flags.none());
}

TEST_CASE("Flags: contains asks for all, any for at least one", "[base][flags]")
{
    auto const flags = Fruits { Fruit::Apple, Fruit::Banana };

    CHECK(flags.contains(Fruit::Apple));
    CHECK(flags.contains(Fruits { Fruit::Apple, Fruit::Banana }));
    CHECK(!flags.contains(AllFruits));

    CHECK(flags.any(Fruits { Fruit::Banana, Fruit::Cherry }));
    CHECK(!flags.any(Fruits { Fruit::Cherry }));
}

// Every compound operator must be the assigning form of the binary one. `&=` was `disable()`:
// it cleared the named flag instead of intersecting, so it computed the complement of what
// `&` computes — the difference between "keep only Apple" and "keep everything but Apple".
TEST_CASE("Flags: a compound operator equals its binary form", "[base][flags]")
{
    SECTION("operator&= intersects, exactly as operator& does")
    {
        auto const both = Fruits { Fruit::Apple, Fruit::Banana };

        auto intersected = both;
        intersected &= Fruit::Apple;
        CHECK(intersected == (both & Fruits { Fruit::Apple }));
        CHECK(intersected == Fruits { Fruit::Apple });

        auto intersectedSet = AllFruits;
        intersectedSet &= both;
        CHECK(intersectedSet == (AllFruits & both));
        CHECK(intersectedSet == both);

        auto disjoint = Fruits { Fruit::Cherry };
        disjoint &= Fruit::Apple;
        CHECK(disjoint.none());
    }

    SECTION("operator|= unites, exactly as operator| does")
    {
        auto united = Fruits { Fruit::Apple };
        united |= Fruit::Banana;
        CHECK(united == (Fruits { Fruit::Apple } | Fruit::Banana));

        auto unitedSet = Fruits { Fruit::Apple };
        unitedSet |= Fruits { Fruit::Banana, Fruit::Cherry };
        CHECK(unitedSet == (Fruits { Fruit::Apple } | Fruits { Fruit::Banana, Fruit::Cherry }));
        CHECK(unitedSet == AllFruits);
    }
}

TEST_CASE("Flags: with, without and intersect leave the receiver alone", "[base][flags]")
{
    auto const flags = Fruits { Fruit::Apple, Fruit::Banana };

    CHECK(flags.with(Fruit::Cherry) == AllFruits);
    CHECK(flags.with(Fruits { Fruit::Cherry }) == AllFruits);
    CHECK(flags.without(Fruits { Fruit::Banana }) == Fruits { Fruit::Apple });
    CHECK(flags.intersect(AllFruits) == flags);
    CHECK(flags.intersect(Fruits { Fruit::Cherry }).none());

    // None of the above wrote through.
    CHECK(flags == (Fruits { Fruit::Apple, Fruit::Banana }));
}

TEST_CASE("Flags: fromValue and value round-trip", "[base][flags]")
{
    CHECK(Fruits::fromValue(0b101) == (Fruits { Fruit::Apple, Fruit::Cherry }));
    CHECK(Fruits::fromValue(AllFruits.value()) == AllFruits);
}

TEST_CASE("Flags: reduce visits each set flag once", "[base][flags]")
{
    auto const names = AllFruits.reduce(std::string {}, [](std::string acc, Fruit fruit) {
        if (!acc.empty())
            acc += ',';
        acc += std::format("{}", fruit);
        return acc;
    });
    CHECK(names == "Apple,Banana,Cherry");

    auto const count =
        Fruits { Fruit::Apple, Fruit::Cherry }.reduce(0, [](int acc, Fruit) { return acc + 1; });
    CHECK(count == 2);
}

TEST_CASE("Flags: formatting lists the set flags", "[base][flags]")
{
    CHECK(std::format("{}", AllFruits) == "Apple|Banana|Cherry");
    CHECK(std::format("{}", Fruits { Fruit::Banana }) == "Banana");
    CHECK(std::format("{}", Fruits {}).empty());
}
