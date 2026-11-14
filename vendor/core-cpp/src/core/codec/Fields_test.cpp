// SPDX-License-Identifier: Apache-2.0
#include <core/Flags.hpp>
#include <core/codec/Fields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace core::codec;

namespace
{

enum class Mood : uint8_t
{
    Calm,
    Excited,
    Asleep,
};

enum class Trait : uint8_t
{
    Fast = 0b001,
    Loud = 0b010,
    Red = 0b100,
};

struct Inner
{
    int32_t count = 0;
    std::string name;

    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.count);
        visitor(2, self.name);
    }
    bool operator==(Inner const&) const = default;
};

struct Outer
{
    bool flag = false;
    int64_t signedValue = 0;
    uint16_t narrow = 0;
    double ratio = 0.0;
    Mood mood = Mood::Calm;
    core::Flags<Trait> traits;
    std::chrono::milliseconds timeout {};
    std::vector<uint8_t> blob;
    std::vector<Inner> inners;
    std::optional<Inner> maybe;
    std::map<std::string, int32_t> table;
    std::variant<int32_t, std::string> either;
    Inner nested;

    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.flag);
        visitor(2, self.signedValue);
        visitor(3, self.narrow);
        visitor(4, self.ratio);
        visitor(5, self.mood);
        visitor(6, self.traits);
        visitor(7, self.timeout);
        visitor(8, self.blob);
        visitor(9, self.inners);
        visitor(10, self.maybe);
        visitor(11, self.table);
        visitor(12, self.either);
        visitor(13, self.nested);
    }
    bool operator==(Outer const&) const = default;
};

/// The older shape of Outer: a reader of this skips what the newer one wrote.
struct OuterV0
{
    bool flag = true;
    int64_t signedValue = 0;

    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.flag);
        visitor(2, self.signedValue);
    }
};

struct Foreign
{
    int32_t a = 0;
    int32_t b = 0;
    bool operator==(Foreign const&) const = default;
};

struct ForeignHooks
{
};

template <typename Visitor, typename T>
    requires std::same_as<std::remove_const_t<T>, Foreign>
void visitForeignFields(Visitor& visitor, T& value)
{
    visitor(1, value.a);
    visitor(2, value.b);
}

struct HoldsForeign
{
    Foreign foreign;
    std::vector<Foreign> many;
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.foreign);
        visitor(2, self.many);
    }
    bool operator==(HoldsForeign const&) const = default;
};

/// @return A stream of the one varint field @p id, holding @p value.
[[nodiscard]] std::vector<uint8_t> oneVarintField(uint32_t id, uint64_t value)
{
    auto writer = Writer {};
    writer.varint(fieldKey(id, WireKind::Varint));
    writer.varint(value);
    return std::move(writer).take();
}

[[nodiscard]] std::vector<uint8_t> encode(auto const& value)
{
    auto writer = Writer {};
    encodeFields(writer, value);
    return std::move(writer).take();
}

} // namespace

template <>
struct core::codec::EnumWire<Mood>
{
    static constexpr auto Values = std::array { Mood::Calm, Mood::Excited, Mood::Asleep };
};

template <>
struct core::codec::FlagWire<Trait>
{
    static constexpr auto Values = std::array { Trait::Fast, Trait::Loud, Trait::Red };
};

TEST_CASE("codec.Fields: a struct of every kind round-trips", "[codec]")
{
    auto const original = Outer {
        .flag = true,
        .signedValue = -1234567890123,
        .narrow = 65535,
        .ratio = 2.5,
        .mood = Mood::Asleep,
        .traits = core::Flags<Trait> { Trait::Fast } | Trait::Red,
        .timeout = std::chrono::milliseconds { 1500 },
        .blob = { 1, 2, 3, 255 },
        .inners = { Inner { 1, "one" }, Inner { 2, "two" } },
        .maybe = Inner { 9, "nine" },
        .table = { { "b", 2 }, { "a", 1 } },
        .either = std::string { "text" },
        .nested = Inner { 7, "seven" },
    };
    auto const bytes = encode(original);
    auto decoded = Outer {};
    auto const result = decodeFields(bytes, decoded);
    REQUIRE(result.has_value());
    CHECK(decoded == original);
}

TEST_CASE("codec.Fields: equal maps have equal bytes whatever their order", "[codec]")
{
    auto first = Outer {};
    first.table = { { "x", 1 }, { "y", 2 }, { "z", 3 } };
    auto second = Outer {};
    second.table.emplace("z", 3);
    second.table.emplace("y", 2);
    second.table.emplace("x", 1);
    CHECK(encode(first) == encode(second));
}

TEST_CASE("codec.Fields: an older reader skips the fields it does not know", "[codec]")
{
    auto outer = Outer {};
    outer.flag = true;
    outer.signedValue = -5;
    outer.inners = { Inner { 1, "x" } };
    outer.nested = Inner { 3, "y" };
    auto older = OuterV0 { .flag = false };
    REQUIRE(decodeFields(encode(outer), older).has_value());
    CHECK(older.flag);
    CHECK(older.signedValue == -5);
}

TEST_CASE("codec.Fields: a newer reader leaves what the stream lacks at its default", "[codec]")
{
    auto older = OuterV0 { .flag = true, .signedValue = 42 };
    auto newer = Outer {};
    newer.narrow = 77;
    newer.nested = Inner { 5, "kept" };
    REQUIRE(decodeFields(encode(older), newer).has_value());
    CHECK(newer.flag);
    CHECK(newer.signedValue == 42);
    CHECK(newer.narrow == 77);
    CHECK(newer.nested.name == "kept");
}

TEST_CASE("codec.Fields: a field twice is refused", "[codec]")
{
    auto writer = Writer {};
    writer.varint(fieldKey(1, WireKind::Varint));
    writer.varint(1);
    writer.varint(fieldKey(1, WireKind::Varint));
    writer.varint(0);
    auto const bytes = std::move(writer).take();
    auto value = OuterV0 {};
    auto const result = decodeFields(bytes, value);
    REQUIRE_FALSE(result.has_value());
    CHECK(result == std::unexpected(DecodeError::MalformedRecord));
}

TEST_CASE("codec.Fields: a kind that does not fit the field is refused", "[codec]")
{
    auto writer = Writer {};
    writer.varint(fieldKey(1, WireKind::Framed));
    writer.string("no");
    auto const bytes = std::move(writer).take();
    auto value = OuterV0 {};
    CHECK(decodeFields(bytes, value) == std::unexpected(DecodeError::MalformedRecord));
}

TEST_CASE("codec.Fields: values out of range are refused", "[codec]")
{
    SECTION("an enumerator the table lacks")
    {
        auto value = Outer {};
        CHECK(decodeFields(oneVarintField(5, 3), value) == std::unexpected(DecodeError::InvalidValue));
    }
    SECTION("a flag bit the table lacks")
    {
        auto value = Outer {};
        CHECK(decodeFields(oneVarintField(6, 0b1000), value) == std::unexpected(DecodeError::InvalidValue));
    }
    SECTION("a bool that is neither 0 nor 1")
    {
        auto value = Outer {};
        CHECK(decodeFields(oneVarintField(1, 2), value) == std::unexpected(DecodeError::InvalidValue));
    }
    SECTION("an integer wider than its field")
    {
        auto value = Outer {};
        CHECK(decodeFields(oneVarintField(3, 65536), value) == std::unexpected(DecodeError::InvalidValue));
    }
}

TEST_CASE("codec.Fields: a truncated stream is refused", "[codec]")
{
    auto full = Outer {};
    full.nested = Inner { 1, "abcdef" };
    auto const bytes = encode(full);
    for (auto const cut: { size_t { 1 }, bytes.size() / 2, bytes.size() - 1 })
    {
        auto value = Outer {};
        auto const result = decodeFields(std::span { bytes }.first(cut), value);
        // A cut at a field boundary is a shorter, valid stream; any other cut must be refused.
        if (!result)
            CHECK(result == std::unexpected(DecodeError::Truncated));
    }
    auto value = Outer {};
    CHECK_FALSE(decodeFields(std::span { bytes }.first(bytes.size() - 1), value).has_value());
}

TEST_CASE("codec.Fields: the decode budget refuses a stream that asks for more than it is", "[codec]")
{
    auto budget = DecodeBudget { 10 };
    CHECK(budget.spend(DecodeBudget::Allowance));
    CHECK_FALSE(budget.spend((DecodeBudget::BytesPerStreamByte * 10) + 1));
    CHECK(budget.spend(DecodeBudget::BytesPerStreamByte * 10));
    CHECK_FALSE(budget.spend(1));
}

TEST_CASE("codec.Fields: foreign types are named by the hooks tag's overloads", "[codec]")
{
    using HookedEncoder = BasicFieldEncoder<ForeignHooks>;
    using HookedDecoder = BasicFieldDecoder<ForeignHooks>;
    auto const original = HoldsForeign { .foreign = Foreign { 1, 2 }, .many = { Foreign { 3, 4 } } };
    auto writer = Writer {};
    auto encoder = HookedEncoder { writer };
    encoder.writeFields(original);
    auto const bytes = std::move(writer).take();

    auto decoded = HoldsForeign {};
    auto budget = DecodeBudget { bytes.size() };
    auto decoder = HookedDecoder { bytes, budget };
    decoder.readFields(decoded);
    REQUIRE(decoder.done().has_value());
    CHECK(decoded == original);
}
