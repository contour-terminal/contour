// SPDX-License-Identifier: Apache-2.0
//
// The Canonical ABI codec: its layouts against what the Canonical ABI computes for the world's types,
// its round trips through a fake linear memory, its refusal of whatever a guest could forge, and its
// round trips through the echo and spawner guests, which wit-bindgen built from the world.
#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Json.hpp>
#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/ControlCodec.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/StatuslineWorld.hpp>
#include <vtextension/testing/FakeGuestMemory.hpp>
#include <vtextension/testing/Harness.hpp>

#ifdef VTEXTENSION_TEST_MODULE_DIR
    #include <vtextension/testing/TestModules.hpp>
#endif

using namespace std::string_view_literals;
using namespace vtextension;
namespace abi = vtextension::abi;
using vtextension::testing::errorOf;

namespace
{

using FakeMemory = testing::FakeGuestMemory;

/// Stores @p value into a fresh memory and loads it back.
template <typename T>
[[nodiscard]] T roundTrip(T const& value)
{
    auto memory = FakeMemory {};
    auto const address = abi::storeNew(memory, value);
    REQUIRE(address.has_value());
    auto loaded = abi::Codec<T>::load(memory, *address);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}

/// Lowers @p value to flat values and lifts it back, checking the count of flat values on the way.
template <typename T>
[[nodiscard]] T flatRoundTrip(T const& value)
{
    auto memory = FakeMemory {};
    auto flat = abi::FlatValues {};
    REQUIRE(abi::Codec<T>::lowerFlat(memory, value, flat).has_value());
    REQUIRE(flat.size() == abi::Codec<T>::FlatCount);
    auto reader = abi::FlatReader(flat);
    auto lifted = abi::Codec<T>::liftFlat(memory, reader);
    REQUIRE(lifted.has_value());
    return std::move(*lifted);
}

/// One of every event, each with a payload that is not its type's default.
[[nodiscard]] std::vector<Event> everyEvent()
{
    return {
        TitleChanged { .title = "vim" },
        CwdChanged { .cwd = "/home/u/src" },
        ForegroundProcessChanged { .name = "make" },
        InputModeChanged { .mode = InputMode::VisualLine },
        TabsChanged { .tabs = { Tab { .title = "one", .activity = TabActivity::Inactive },
                                Tab { .title = "two", .activity = TabActivity::Active } } },
        UserVarChanged { .name = "branch", .value = "main" },
        ProgressChanged { .progress = Progress { .state = ProgressState::Warning, .percent = 42 } },
        FocusChanged { .state = FocusState::Focused },
        FontChanged { .metrics = FontMetrics { .pointSize = 13.5F, .cellWidth = 10, .cellHeight = 21 } },
        Resized { .size = SurfaceSize { .cols = 80, .rows = 2 } },
        TimerFired {},
        ReplyBytes { .bytes = { 0x1b, '[', '0', 'n' } },
    };
}

/// A snapshot with every field set.
[[nodiscard]] StateSnapshot fullSnapshot()
{
    return StateSnapshot {
        .title = "title ✓",
        .cwd = "/tmp",
        .foregroundProcess = "zsh",
        .inputMode = InputMode::Hint,
        .tabs = { Tab { .title = "a", .activity = TabActivity::Active }, Tab { .title = "b" } },
        .userVars = { { "k", "v" }, { "x", "=y" } },
        .progress = Progress { .state = ProgressState::Normal, .percent = 7 },
        .focus = FocusState::Focused,
        .size = SurfaceSize { .cols = 120, .rows = 1 },
        .now = std::chrono::sys_time<std::chrono::milliseconds> { std::chrono::milliseconds {
            1'700'000'000'123 } },
        .utcOffset = std::chrono::minutes { -570 },
        .font = FontMetrics { .pointSize = 12.5F, .cellWidth = 9, .cellHeight = 18 },
    };
}

} // namespace

TEST_CASE("CanonicalAbi.layout", "[vtextension]")
{
    // What the Canonical ABI computes for the world's types: natural alignment, fields in order,
    // and a variant's payload after its discriminant at the widest case's alignment.
    STATIC_CHECK(abi::Codec<std::string>::Size == 8);
    STATIC_CHECK(abi::Codec<ByteList>::Size == 8);
    STATIC_CHECK(abi::Codec<Tab>::Size == 12);
    STATIC_CHECK(abi::Codec<Tab>::Alignment == 4);
    STATIC_CHECK(abi::Codec<Progress>::Size == 2);
    STATIC_CHECK(abi::Codec<Progress>::Alignment == 1);
    STATIC_CHECK(abi::Codec<SurfaceSize>::Size == 8);
    STATIC_CHECK(abi::Codec<std::pair<std::string, std::string>>::Size == 16);
    // Nine flags take a u16.
    STATIC_CHECK(abi::Codec<EventKinds>::Size == 2);
    STATIC_CHECK(abi::Codec<FontMetrics>::Size == 12);
    STATIC_CHECK(abi::Codec<FontMetrics>::Alignment == 4);
    // What the flat-join guard of Codec<float> in StatuslineWorld.hpp sees: f32s wherever they are nested.
    STATIC_CHECK(abi::HoldsFloat<FontMetrics>);
    STATIC_CHECK(abi::HoldsFloat<StateSnapshot>);
    STATIC_CHECK(abi::HoldsFloat<Event>);
    STATIC_CHECK(abi::HoldsFloat<std::vector<FontMetrics>>);
    STATIC_CHECK_FALSE(abi::HoldsFloat<Tab>);
    STATIC_CHECK_FALSE(abi::HoldsFloat<abi::CommandResult>);
    STATIC_CHECK(abi::Codec<InputMode>::Size == 1);

    // state-snapshot: title 0, cwd 8, foreground-process 16, input-mode 24, tabs 28, user-vars 36,
    // progress 44, focused 46, size 48, unix-time-ms 56 (aligned to 8), utc-offset-minutes 64, font 68 (three
    // 4-byte fields); padded to 80.
    STATIC_CHECK(abi::Codec<StateSnapshot>::Offsets
                 == std::array<uint32_t, 12> { 0, 8, 16, 24, 28, 36, 44, 46, 48, 56, 64, 68 });
    STATIC_CHECK(abi::Codec<StateSnapshot>::Size == 80);
    STATIC_CHECK(abi::Codec<StateSnapshot>::Alignment == 8);

    // command-output: exit-code 0, stdout 4, stderr 12; the result puts it after a byte, at 4.
    STATIC_CHECK(abi::Codec<abi::CommandOutput>::Size == 20);
    STATIC_CHECK(abi::Codec<abi::CommandResult>::Size == 24);
    STATIC_CHECK(abi::Codec<abi::CommandResult>::Alignment == 4);

    // event: the widest case is user-var's tuple<string, string>, four i32 flat or 16 bytes.
    STATIC_CHECK(abi::Codec<Event>::PayloadOffset == 4);
    STATIC_CHECK(abi::Codec<Event>::Size == 20);
    STATIC_CHECK(abi::Codec<Event>::FlatCount == 5);
}

TEST_CASE("CanonicalAbi.roundTrip", "[vtextension]")
{
    CHECK(roundTrip(std::string { "hello, wörld" }) == "hello, wörld");
    CHECK(roundTrip(std::string {}).empty());
    CHECK(roundTrip(ByteList { 0, 1, 0xFF }) == ByteList { 0, 1, 0xFF });
    CHECK(roundTrip(std::vector<std::string> { "a", "", "ccc" })
          == std::vector<std::string> { "a", "", "ccc" });
    auto const config = ConfigMap { { "text", "hi" }, { "speed", "2" } };
    CHECK(roundTrip(config) == config);
    CHECK(roundTrip(fullSnapshot()) == fullSnapshot());
    CHECK(roundTrip(EventKinds { EventKind::Cwd, EventKind::Focus })
          == EventKinds { EventKind::Cwd, EventKind::Focus });
    CHECK(roundTrip(RenderRequest::Render) == RenderRequest::Render);
    CHECK(roundTrip(int32_t { -3 }) == -3);

    auto const output =
        abi::CommandOutput { .exitCode = 1, .standardOutput = { 'o' }, .standardError = { 'e' } };
    CHECK(roundTrip(abi::CommandResult { output }) == abi::CommandResult { output });
    CHECK(roundTrip(abi::CommandResult { std::unexpected(abi::CommandError::TimedOut) })
          == abi::CommandResult { std::unexpected(abi::CommandError::TimedOut) });

    for (auto const& event: everyEvent())
    {
        INFO("event alternative " << event.index());
        CHECK(roundTrip(event) == event);
        CHECK(flatRoundTrip(event) == event);
    }
}

TEST_CASE("CanonicalAbi.flatEvent", "[vtextension]")
{
    // The discriminant comes first; a narrower payload leaves the rest of the five slots zero.
    auto memory = FakeMemory {};
    auto flat = abi::FlatValues {};
    REQUIRE(
        abi::Codec<Event>::lowerFlat(memory, Event { FocusChanged { .state = FocusState::Focused } }, flat));
    CHECK(flat == abi::FlatValues { 7, 1, 0, 0, 0 });

    flat.clear();
    REQUIRE(abi::Codec<Event>::lowerFlat(memory, Event { TimerFired {} }, flat));
    CHECK(flat == abi::FlatValues { 10, 0, 0, 0, 0 });

    // An f32 shares its position with the i32s of other cases, so it travels as the i32 of its bits.
    flat.clear();
    REQUIRE(abi::Codec<Event>::lowerFlat(
        memory,
        Event { FontChanged { .metrics =
                                  FontMetrics { .pointSize = 13.5F, .cellWidth = 10, .cellHeight = 21 } } },
        flat));
    CHECK(flat == abi::FlatValues { 8, 0x4158'0000, 10, 21, 0 });

    flat.clear();
    REQUIRE(
        abi::Codec<Event>::lowerFlat(memory, Event { UserVarChanged { .name = "ab", .value = "c" } }, flat));
    REQUIRE(flat.size() == 5);
    CHECK(flat[0] == 5);
    CHECK(flat[2] == 2);
    CHECK(flat[4] == 1);
}

TEST_CASE("CanonicalAbi.lift.outOfBounds", "[vtextension]")
{
    auto memory = FakeMemory {};

    SECTION("a pair outside memory")
    {
        CHECK(errorOf(abi::Codec<std::string>::load(memory, FakeMemory::Size - 4))
              == ExtensionError::Trapped);
        CHECK(errorOf(abi::Codec<std::string>::load(memory, 0xFFFF'FFFC)) == ExtensionError::Trapped);
    }

    SECTION("a string outside memory")
    {
        memory.poke(16, FakeMemory::Size - 2);
        memory.poke(20, 4);
        CHECK(errorOf(abi::Codec<std::string>::load(memory, 16)) == ExtensionError::Trapped);
    }

    SECTION("a string that wraps the address space")
    {
        memory.poke(16, 0xFFFF'FFF0);
        memory.poke(20, 0x20);
        CHECK(errorOf(abi::Codec<std::string>::load(memory, 16)) == ExtensionError::Trapped);
    }

    SECTION("a list whose byte length overflows 32 bits")
    {
        memory.poke(16, 64);
        memory.poke(20, 0x1000'0001); // 16-byte elements: 0x1'0000'0010 bytes
        CHECK(errorOf(abi::Codec<ConfigMap>::load(memory, 16)) == ExtensionError::Trapped);
    }

    SECTION("a list whose element points outside memory")
    {
        memory.poke(16, 64); // one tuple<string, string> at 64 ...
        memory.poke(20, 1);
        memory.poke(64, 0xFFFF'0000); // ... whose first string is nowhere
        memory.poke(68, 1);
        CHECK(errorOf(abi::Codec<ConfigMap>::load(memory, 16)) == ExtensionError::Trapped);
    }

    SECTION("flat values that run out")
    {
        auto const flat = std::array<uint32_t, 1> { 16 };
        auto reader = abi::FlatReader(flat);
        CHECK(errorOf(abi::Codec<std::string>::liftFlat(memory, reader)) == ExtensionError::Trapped);
    }
}

TEST_CASE("CanonicalAbi.lift.invalid", "[vtextension]")
{
    auto memory = FakeMemory {};

    SECTION("a string that is not UTF-8")
    {
        REQUIRE(abi::Codec<ByteList>::store(memory, 16, ByteList { 'a', 0xC0, 0x80 }));
        CHECK(errorOf(abi::Codec<std::string>::load(memory, 16)) == ExtensionError::Trapped);
        // The same bytes are a fine list<u8>.
        CHECK(abi::Codec<ByteList>::load(memory, 16).value() == ByteList { 'a', 0xC0, 0x80 });
    }

    SECTION("an unknown enum case")
    {
        auto const flat = std::array<uint32_t, 1> { 5 };
        auto reader = abi::FlatReader(flat);
        CHECK(errorOf(abi::Codec<LogLevel>::liftFlat(memory, reader)) == ExtensionError::Trapped);
    }

    SECTION("an unknown variant case")
    {
        auto const flat = std::array<uint32_t, 5> { 12, 0, 0, 0, 0 };
        auto reader = abi::FlatReader(flat);
        CHECK(errorOf(abi::Codec<Event>::liftFlat(memory, reader)) == ExtensionError::Trapped);
    }

    SECTION("a bool is any non-zero byte")
    {
        memory.poke(16, 0xFE);
        CHECK(abi::Codec<FocusState>::load(memory, 16).value() == FocusState::Focused);
    }

    SECTION("flags drop the bits the world does not define")
    {
        // Bits 0, 7 and 8 are title, focus and font; the nine members define nothing above them.
        auto const flat = std::array<uint32_t, 1> { 0xFFFF'FF81 };
        auto reader = abi::FlatReader(flat);
        CHECK(abi::Codec<EventKinds>::liftFlat(memory, reader).value()
              == EventKinds { EventKind::Title, EventKind::Focus, EventKind::Font });
    }
}

TEST_CASE("CanonicalAbi.loadByteString", "[vtextension]")
{
    auto memory = FakeMemory {};
    REQUIRE(abi::Codec<ByteList>::store(memory, 16, ByteList { 'a', 'b', 'c' }));

    CHECK(abi::loadByteString(memory, 16, 3).value() == "abc");
    CHECK(errorOf(abi::loadByteString(memory, 16, 2)) == ExtensionError::OutputTooLarge);

    // The limit is checked before the bounds: a forged length is too large, whatever it points at.
    memory.poke(24, 0xFFFF'0000);
    memory.poke(28, 0xFFFF'FFFF);
    CHECK(errorOf(abi::loadByteString(memory, 24, 1024)) == ExtensionError::OutputTooLarge);
    memory.poke(28, 16);
    CHECK(errorOf(abi::loadByteString(memory, 24, 1024)) == ExtensionError::Trapped);
}

TEST_CASE("CanonicalAbi.commandErrorFor", "[vtextension]")
{
    CHECK(abi::commandErrorFor(ExtensionError::CommandNotDeclared) == abi::CommandError::NotDeclared);
    CHECK(abi::commandErrorFor(ExtensionError::ArgumentRejected) == abi::CommandError::NotDeclared);
    CHECK(abi::commandErrorFor(ExtensionError::PermissionPending) == abi::CommandError::Pending);
    CHECK(abi::commandErrorFor(ExtensionError::PermissionDenied) == abi::CommandError::Denied);
    CHECK(abi::commandErrorFor(ExtensionError::DeadlineExceeded) == abi::CommandError::TimedOut);
    CHECK(abi::commandErrorFor(ExtensionError::Trapped) == abi::CommandError::Failed);
    CHECK(abi::commandErrorFor(ExtensionError::RuntimeUnavailable) == abi::CommandError::Failed);
}

TEST_CASE("CanonicalAbi.lift.budget", "[vtextension]")
{
    // A lift spends its budget before it allocates: neither many elements nor many aliases of one
    // large string make the host allocate past it.
    SECTION("the elements of a long list")
    {
        auto memory = FakeMemory { 1024 };
        memory.poke(16, 64);
        memory.poke(20, 1000); // 1000 empty tuples: zeros are (0, 0, 0, 0)
        CHECK(errorOf(abi::Codec<ConfigMap>::load(memory, 16)) == ExtensionError::Trapped);
    }

    SECTION("elements that all alias one string")
    {
        auto memory = FakeMemory { 4096 };
        memory.poke(16, 1024); // four tuples at 1024 ...
        memory.poke(20, 4);
        for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 8 }))
        {
            memory.poke(1024 + (index * 8), 2048); // ... every string the same 1000 zeros at 2048
            memory.poke(1028 + (index * 8), 1000);
        }
        CHECK(errorOf(abi::Codec<ConfigMap>::load(memory, 16)) == ExtensionError::Trapped);

        // The same list under a budget it fits is fine.
        auto generous = FakeMemory {};
        generous.poke(16, 1024);
        generous.poke(20, 4);
        for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 8 }))
        {
            generous.poke(1024 + (index * 8), 2048);
            generous.poke(1028 + (index * 8), 1000);
        }
        CHECK(abi::Codec<ConfigMap>::load(generous, 16).value().size() == 4);
    }
}

TEST_CASE("CanonicalAbi.wrappingRecord", "[vtextension]")
{
    // A record near the end of the address space is refused whole: no field's address may wrap round
    // to the start of memory.
    auto memory = FakeMemory {};
    CHECK(errorOf(abi::Codec<StateSnapshot>::store(memory, 0xFFFF'FFF0, StateSnapshot {}))
          == ExtensionError::Trapped);
    CHECK(errorOf(abi::Codec<StateSnapshot>::load(memory, 0xFFFF'FFF0)) == ExtensionError::Trapped);
    CHECK(errorOf(abi::Codec<Event>::load(memory, 0xFFFF'FFF8)) == ExtensionError::Trapped);
}

namespace
{

/// What a FetchCountingMemory does to guest bytes once read() copied them.
enum class AfterRead : uint8_t
{
    Kept,      ///< Nothing.
    Corrupted, ///< Flips every bit of them, as a guest scribbling its memory meanwhile would.
};

/// Whether a FetchCountingMemory is being written to or read from.
enum class FetchPhase : uint8_t
{
    Storing, ///< writable() is the memory: a sample is being lowered.
    Loading, ///< writable() is a decoy: only read() reaches the memory, and counts what it copies.
};

/// A guest memory that counts how often lifting reads each of its bytes. While loading, writable() -- which
/// decoders may use to prove a range, but never to read -- hands out a decoy of other bytes, so a decoder
/// that reads through it lifts something else.
class FetchCountingMemory final: public abi::IGuestMemory
{
  public:
    static constexpr uint32_t Size = 64 * 1024;

    /// @param afterRead What read() does to the bytes it copied.
    explicit FetchCountingMemory(AfterRead afterRead = AfterRead::Kept): _afterRead { afterRead } {}

    /// From now on, writable() hands out the decoy and read() counts.
    void startLoading() noexcept { _phase = FetchPhase::Loading; }

    /// @return How often the most read byte was read.
    [[nodiscard]] uint32_t mostReads() const { return std::ranges::max(_reads); }

    /// @return How many bytes were read at all.
    [[nodiscard]] std::size_t bytesRead() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(_reads, [](uint32_t reads) { return reads > 0; }));
    }

    [[nodiscard]] abi::Result<void> read(abi::Address address, std::span<std::byte> into) override
    {
        if (uint64_t { address } + into.size() > Size)
            return std::unexpected(ExtensionError::Trapped);
        auto const source = std::span { _bytes }.subspan(address, into.size());
        std::ranges::copy(source, into.begin());
        for (auto& reads: std::span { _reads }.subspan(address, into.size()))
            ++reads;
        if (_afterRead == AfterRead::Corrupted)
            for (auto& byte: source)
                byte = ~byte;
        return {};
    }

    [[nodiscard]] abi::Result<std::span<std::byte>> writable(abi::Address address, uint32_t size) override
    {
        if (uint64_t { address } + size > Size)
            return std::unexpected(ExtensionError::Trapped);
        auto& bytes = _phase == FetchPhase::Storing ? _bytes : _decoy;
        return std::span { bytes }.subspan(address, size);
    }

    [[nodiscard]] abi::Result<abi::Address> allocate(uint32_t size, uint32_t alignment) override
    {
        auto const address = abi::alignTo(_next, alignment);
        if (uint64_t { address } + size > Size)
            return std::unexpected(ExtensionError::OutOfMemory);
        _next = address + size;
        return address;
    }

    [[nodiscard]] abi::Result<void> chargeLift(uint64_t /*bytes*/) override { return {}; }

  private:
    AfterRead _afterRead;
    FetchPhase _phase = FetchPhase::Storing;
    std::vector<std::byte> _bytes = std::vector<std::byte>(Size);
    std::vector<std::byte> _decoy = std::vector<std::byte>(Size, std::byte { 0xA5 });
    std::vector<uint32_t> _reads = std::vector<uint32_t>(Size);
    abi::Address _next = 8; ///< Address 0 stays unused, as a guest's null.
};

/// The request type of each control method, in the catalog's order.
using ControlRequests = decltype(std::apply(
    [](auto const&... row) { return std::tuple<typename std::remove_cvref_t<decltype(row)>::Request...> {}; },
    vtcontrol::Methods));

/// Every type the host lifts out of a guest: what host functions take, what exports return, and the requests
/// of the control verbs.
using LiftedTypes = decltype(std::tuple_cat(
    std::tuple<std::string, ByteList, ConfigMap, LogLevel, EventKinds, RenderRequest> {},
    ControlRequests {}));

/// @return A value of @p T that is not its default, where it has one: for a control request, the example
///         of the first method taking it.
template <typename T>
[[nodiscard]] T sampleOf()
{
    if constexpr (std::is_same_v<T, std::string>)
        return "grüße, ☃";
    else if constexpr (std::is_same_v<T, ByteList>)
        return ByteList { 0x00, 0x01, 0x7F, 0x80, 0xFF };
    else if constexpr (std::is_same_v<T, ConfigMap>)
        return ConfigMap { { "format", "{cwd}" }, { "größe", "" }, { "", "zwölf" } };
    else if constexpr (std::is_same_v<T, LogLevel>)
        return LogLevel::Warn;
    else if constexpr (std::is_same_v<T, EventKinds>)
        return EventKinds { EventKind::Title, EventKind::Focus, EventKind::Font };
    else if constexpr (std::is_same_v<T, RenderRequest>)
        return RenderRequest::Render;
    else
    {
        auto sample = std::optional<T> {};
        std::apply(
            [&](auto const&... row) {
                std::ignore =
                    ((std::is_same_v<typename std::remove_cvref_t<decltype(row)>::Request, T>
                      && (sample = vtcontrol::decodeJson<T>(row.requestExample).value()).has_value())
                     || ...);
            },
            vtcontrol::Methods);
        REQUIRE(sample.has_value());
        return *std::move(sample);
    }
}

/// @return Whether @p a and @p b are the same value: by `==` where @p T has it, else by their JSON.
template <typename T>
[[nodiscard]] bool sameValue(T const& a, T const& b)
{
    if constexpr (std::equality_comparable<T>)
        return a == b;
    else
        return vtcontrol::encodeJson(a) == vtcontrol::encodeJson(b);
}

} // namespace

TEMPLATE_LIST_TEST_CASE("GuestReader.fetchesEachByteOnce", "[vtextension]", LiftedTypes)
{
    // A lift reads guest memory only through read(), and no byte twice: what it decodes is one copy, whatever
    // the guest's memory holds meanwhile.
    auto const sample = sampleOf<TestType>();
    auto memory = FetchCountingMemory {};
    auto const address = abi::storeNew(memory, sample);
    REQUIRE(address.has_value());

    memory.startLoading();
    auto const loaded = abi::Codec<TestType>::load(memory, *address);
    REQUIRE(loaded.has_value());
    CHECK(sameValue(*loaded, sample));
    CHECK(memory.mostReads() <= 1);
    // Something was read, unless there is nothing to: a request without fields.
    CHECK((memory.bytesRead() > 0 || abi::Codec<TestType>::Size == 0));
}

TEMPLATE_LIST_TEST_CASE("GuestReader.flatLiftsFetchEachByteOnce", "[vtextension]", LiftedTypes)
{
    // What a host function or a small control request lifts from its flat parameters: the pointers they
    // carry are followed through read() alone, and no byte is read twice.
    auto const sample = sampleOf<TestType>();
    auto memory = FetchCountingMemory {};
    auto flat = abi::FlatValues {};
    REQUIRE(abi::Codec<TestType>::lowerFlat(memory, sample, flat).has_value());

    memory.startLoading();
    auto reader = abi::FlatReader(flat);
    auto const lifted = abi::Codec<TestType>::liftFlat(memory, reader);
    REQUIRE(lifted.has_value());
    CHECK(sameValue(*lifted, sample));
    CHECK(memory.mostReads() <= 1);
}

TEST_CASE("GuestReader.mutationAfterReadChangesNothing", "[vtextension]")
{
    // Every byte is flipped once read() copied it: a lift that read any byte again would see the flip.
    SECTION("a string stays the first copy")
    {
        auto const sample = std::string { "the first copy" };
        auto memory = FetchCountingMemory { AfterRead::Corrupted };
        auto const address = abi::storeNew(memory, sample);
        REQUIRE(address.has_value());
        memory.startLoading();
        CHECK(abi::Codec<std::string>::load(memory, *address).value() == sample);
    }

    SECTION("a list keeps its first length")
    {
        auto const sample = sampleOf<ConfigMap>();
        auto memory = FetchCountingMemory { AfterRead::Corrupted };
        auto const address = abi::storeNew(memory, sample);
        REQUIRE(address.has_value());
        memory.startLoading();
        auto const loaded = abi::Codec<ConfigMap>::load(memory, *address);
        REQUIRE(loaded.has_value());
        CHECK(loaded->size() == sample.size());
        CHECK(*loaded == sample);
    }

    SECTION("an extension's output stays the first copy")
    {
        auto const sample = ByteList { 'o', 'u', 't' };
        auto memory = FetchCountingMemory { AfterRead::Corrupted };
        auto const address = abi::storeNew(memory, sample);
        REQUIRE(address.has_value());
        memory.startLoading();
        CHECK(abi::loadByteString(memory, *address, sample.size()).value() == "out");
    }
}

#ifdef VTEXTENSION_TEST_MODULE_DIR
using vtextension::testing::readTestModule;
using vtextension::testing::RunningGuest;
using vtextension::testing::surface;

TEST_CASE("CanonicalAbi.echo.config", "[vtextension][guest]")
{
    // list<tuple<string, string>> in, list<u8> out.
    auto guest = RunningGuest(readTestModule("echo.wasm"));
    CHECK(guest.loadAndRender({ { "color", "red" }, { "text", "hello" } }) == "hello");
}

TEST_CASE("CanonicalAbi.echo.eventKinds", "[vtextension][guest]")
{
    // event-kinds: a u16 for its nine members, a member's bit is its declaration index, and EventKind's first
    // nine bits are the same members.
    auto const bitOf = std::to_array<std::pair<std::string_view, EventKind>>({
        { "title"sv, EventKind::Title },
        { "cwd"sv, EventKind::Cwd },
        { "foreground-process"sv, EventKind::ForegroundProcess },
        { "input-mode"sv, EventKind::InputMode },
        { "tabs"sv, EventKind::Tabs },
        { "user-var"sv, EventKind::UserVar },
        { "progress"sv, EventKind::Progress },
        { "focus"sv, EventKind::Focus },
        { "font"sv, EventKind::Font },
    });
    for (auto const& [name, kind]: bitOf)
    {
        INFO("subscribing to " << name);
        auto guest = RunningGuest(readTestModule("echo.wasm"));
        auto const kinds = guest.instance().load({ { "subscribe", std::string(name) } });
        REQUIRE(kinds.has_value());
        CHECK(*kinds == (AlwaysDelivered | kind));
    }

    auto guest = RunningGuest(readTestModule("echo.wasm"));
    CHECK(guest.instance().load({}).value() == AlwaysDelivered);
}

TEST_CASE("CanonicalAbi.echo.events", "[vtextension][guest]")
{
    // Every case of the event variant, with records, enums, a bool, a list of records, a tuple and a
    // list<u8> among the payloads, as the guest's wit-bindgen code reads them.
    auto const expected = std::array {
        "title:vim"sv,
        "cwd:/home/u/src"sv,
        "foreground-process:make"sv,
        "input-mode:visual-line"sv,
        "tabs:one|two*"sv,
        "user-var:branch=main"sv,
        "progress:warning/42"sv,
        "focus:true"sv,
        "font-changed:13.5pt 10x21"sv,
        "resized:80x2"sv,
        "timer"sv,
        "reply:1b5b306e"sv,
    };
    auto guest = RunningGuest(readTestModule("echo.wasm"));
    REQUIRE(guest.instance().load({ { "events", "describe" } }).has_value());
    auto const events = everyEvent();
    REQUIRE(events.size() == expected.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, events.size()))
    {
        INFO("event alternative " << index);
        CHECK(guest.instance().update(events[index]).value() == RenderRequest::Render);
        CHECK(guest.instance().render(surface(80, 1)).value() == expected.at(index));
    }
}

TEST_CASE("CanonicalAbi.echo.events.followed", "[vtextension][guest]")
{
    // Only the kinds it subscribed to are described: a resize every guest is sent, subscribed or not, leaves
    // the last followed event shown.
    auto guest = RunningGuest(readTestModule("echo.wasm"));
    REQUIRE(
        guest.instance().load({ { "events", "describe-followed" }, { "subscribe", "font" } }).has_value());
    auto const font =
        FontChanged { .metrics = FontMetrics { .pointSize = 14, .cellWidth = 11, .cellHeight = 22 } };
    CHECK(guest.instance().update(font).value() == RenderRequest::Render);
    CHECK(guest.instance().update(Resized { .size = SurfaceSize { .cols = 80, .rows = 1 } }).value()
          == RenderRequest::None);
    CHECK(guest.instance().update(TitleChanged { .title = "vim" }).value() == RenderRequest::None);
    CHECK(guest.instance().render(surface(80, 1)).value() == "font-changed:14pt 11x22");

    // A reply is not described, but still shown as hex, as without description.
    CHECK(guest.instance().update(ReplyBytes { .bytes = { 0x1b, '[', '0', 'n' } }).value()
          == RenderRequest::Render);
    CHECK(guest.instance().render(surface(80, 1)).value() == "1b5b306e");
}

TEST_CASE("CanonicalAbi.echo.state", "[vtextension][guest]")
{
    // The state-snapshot record, lowered into the guest's memory at the address it passes.
    auto guest = RunningGuest(readTestModule("echo.wasm"));
    guest.services.snapshot = fullSnapshot();
    CHECK(guest.loadAndRender({ { "state", "describe" } })
          == "title ✓;/tmp;zsh;hint;a*|b;k=v,x==y;normal/7;true;120x1;1700000000123;-570;12.5pt 9x18");
}

TEST_CASE("CanonicalAbi.echo.log", "[vtextension][guest]")
{
    // An enum and a string, lifted from the import's flat parameters.
    auto guest = RunningGuest(readTestModule("echo.wasm"));
    CHECK(guest.loadAndRender({ { "text", "x" }, { "log", "loaded" } }) == "x");
    REQUIRE(guest.services.logged.size() == 1);
    CHECK(guest.services.logged.front()
          == vtextension::testing::LoggedMessage { .level = LogLevel::Info, .message = "loaded" });
}

TEST_CASE("CanonicalAbi.echo.reply", "[vtextension][guest]")
{
    auto guest = RunningGuest(readTestModule("echo.wasm"));
    CHECK(guest.loadAndRender({ { "text", "hi" } }) == "hi");

    // Events other than a reply leave the output alone and ask for no render.
    CHECK(guest.instance().update(TitleChanged { .title = "vim" }).value() == RenderRequest::None);
    CHECK(guest.instance().render(surface(10, 1)).value() == "hi");

    // A reply asks for a render, which shows the reply bytes as hex.
    CHECK(guest.instance().update(ReplyBytes { .bytes = { 0x1b, '[', '0', 'n' } }).value()
          == RenderRequest::Render);
    CHECK(guest.instance().render(surface(10, 1)).value() == "1b5b306e");
}

TEST_CASE("CanonicalAbi.spawner.result", "[vtextension][guest]")
{
    // list<tuple<string, string>> in; result<command-output, command-error> out, both arms.
    auto guest = RunningGuest(readTestModule("spawner.wasm"));
    REQUIRE(guest.instance().load({ { "args", "word=hello,other=x=y" } }).has_value());

    guest.services.commandAnswer = "hello\n";
    CHECK(guest.instance().render(surface(80, 1)).value() == "ok:0:hello\n:");
    REQUIRE(guest.services.commands.size() == 1);
    CHECK(guest.services.commands.front().args
          == std::vector<std::pair<std::string, std::string>> { { "word", "hello" }, { "other", "x=y" } });

    guest.services.commandAnswer = std::unexpected(ExtensionError::PermissionDenied);
    CHECK(guest.instance().render(surface(80, 1)).value() == "err:denied");
}
#else
TEST_CASE("CanonicalAbi: the Rust-guest cases", "[vtextension][guest]")
{
    SKIP("the Rust test guests are not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif
