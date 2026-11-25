// SPDX-License-Identifier: Apache-2.0
//
// Decoding what an extension passes while its host scribbles the memory: an instance's memory object, laid
// out with values of every kind the host lifts, is granted to a probe that flips its bytes at random for as
// long as Contour decodes them. Each decode fails as a guest that broke the ABI fails, or yields a value that
// obeys every rule a lift checks; none sees a length it did not check, or bytes it did not validate.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Json.hpp>
#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/ControlCodec.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/StatuslineWorld.hpp>
#include <vtextension/Utf8.hpp>
#include <vtextension/wasm/remote/SharedGuestMemory.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/process/testing/LaunchedHelper.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

using namespace vtextension;
using namespace vtextension::wasm::remote;
namespace abi = vtextension::abi;

namespace
{

/// The size of the object granted, and of the memory it holds.
constexpr auto ObjectBytes = uint64_t { 1024 } * 1024;

/// What the probe seeds its random flips with: the same every run.
constexpr auto MutationSeed = 0x5eed;

/// How many decodes run while the probe scribbles.
constexpr auto Decodes = 100'000;

/// What one decode may spend on the host: what it lifts is charged against it.
constexpr auto LiftBudget = uint64_t { 64 } * 1024;

/// The most bytes an extension's output may hold.
constexpr auto MaxOutputBytes = std::size_t { 4096 };

/// An instance's shared memory as the codec reaches it, as Instance's own does: what lifting reads is one
/// copy of a range bounded by the acknowledged size; allocation bumps through the memory, for laying it out.
class ScribbledMemory final: public abi::IGuestMemory
{
  public:
    /// @param memory The memory, which outlives this.
    explicit ScribbledMemory(SharedGuestMemory& memory) noexcept: _memory { memory } {}

    /// Gives the next decode the full budget.
    void resetLiftBudget() noexcept { _liftBudget = LiftBudget; }

    [[nodiscard]] abi::Result<void> read(abi::Address address, std::span<std::byte> into) override
    {
        if (into.size() > std::numeric_limits<uint32_t>::max())
            return std::unexpected(ExtensionError::Trapped);
        return writable(address, static_cast<uint32_t>(into.size()))
            .transform([&](std::span<std::byte> bytes) {
                if (!bytes.empty())
                    std::memcpy(into.data(), bytes.data(), bytes.size());
            });
    }

    [[nodiscard]] abi::Result<std::span<std::byte>> writable(abi::Address address, uint32_t size) override
    {
        return _memory.view(address, size).transform_error([](vtextension::wasm::MemoryFailure) {
            return ExtensionError::Trapped;
        });
    }

    [[nodiscard]] abi::Result<abi::Address> allocate(uint32_t size, uint32_t alignment) override
    {
        auto const address = abi::alignTo(_next, alignment);
        if (uint64_t { address } + size > _memory.acknowledgedBytes())
            return std::unexpected(ExtensionError::OutOfMemory);
        _next = address + size;
        return address;
    }

    [[nodiscard]] abi::Result<void> chargeLift(uint64_t bytes) override
    {
        if (bytes > _liftBudget)
            return std::unexpected(ExtensionError::Trapped);
        _liftBudget -= bytes;
        return {};
    }

  private:
    SharedGuestMemory& _memory;
    abi::Address _next = 8; ///< Address 0 stays unused, as a guest's null.
    uint64_t _liftBudget = LiftBudget;
};

/// How a decode came out.
enum class Decoded : uint8_t
{
    Inadmissible, ///< A value that breaks a rule a lift checks, or a failure no guest can cause.
    Refused,      ///< Refused as a guest that broke the ABI is: `Trapped`, or `OutOfMemory`.
    Admissible,   ///< A value that obeys every rule a lift checks.
};

/// @return How @p lifted came out, @p obeys judging a value.
template <typename T, typename Obeys>
[[nodiscard]] Decoded judge(abi::Result<T> const& lifted, Obeys const& obeys)
{
    if (!lifted)
        return lifted.error() == ExtensionError::Trapped || lifted.error() == ExtensionError::OutOfMemory
                   ? Decoded::Refused
                   : Decoded::Inadmissible;
    return obeys(*lifted) ? Decoded::Admissible : Decoded::Inadmissible;
}

/// @return Whether @p text is UTF-8 that a lift could have charged.
[[nodiscard]] bool isLiftable(std::string_view text)
{
    return isValidUtf8(text) && text.size() <= LiftBudget;
}

/// One kind of value laid out in the object: how one is stored, and how a decode of one is judged.
struct SampleKind
{
    std::string_view name;
    abi::Result<abi::Address> (*store)(abi::IGuestMemory& memory);
    Decoded (*decode)(abi::IGuestMemory& memory, abi::Address address);
};

/// Every kind of value the probe's object is laid out with, in the order it is.
constexpr auto SampleKinds = std::to_array<SampleKind>({
    { .name = "string",
      .store = [](abi::IGuestMemory& memory) { return abi::storeNew(memory, std::string { "grüße, ☃" }); },
      .decode =
          [](abi::IGuestMemory& memory, abi::Address address) {
              return judge(abi::Codec<std::string>::load(memory, address), isLiftable);
          } },
    { .name = "output",
      .store =
          [](abi::IGuestMemory& memory) { return abi::storeNew(memory, ByteList { 'o', 'u', 't', 0xFF }); },
      .decode =
          [](abi::IGuestMemory& memory, abi::Address address) {
              // Output past its limit is refused as such, before a byte of it is read.
              auto const lifted = abi::loadByteString(memory, address, MaxOutputBytes);
              if (!lifted && lifted.error() == ExtensionError::OutputTooLarge)
                  return Decoded::Refused;
              return judge(lifted, [](std::string const& bytes) { return bytes.size() <= MaxOutputBytes; });
          } },
    { .name = "config map",
      .store =
          [](abi::IGuestMemory& memory) {
              return abi::storeNew(memory, ConfigMap { { "format", "{cwd}" }, { "größe", "zwölf" } });
          },
      .decode =
          [](abi::IGuestMemory& memory, abi::Address address) {
              return judge(abi::Codec<ConfigMap>::load(memory, address), [](ConfigMap const& map) {
                  auto bytes = uint64_t { 0 };
                  for (auto const& [key, value]: map)
                  {
                      if (!isLiftable(key) || !isLiftable(value))
                          return false;
                      bytes += key.size() + value.size();
                  }
                  return bytes + (map.size() * sizeof(ConfigMap::value_type)) <= LiftBudget;
              });
          } },
    { .name = "log level",
      .store = [](abi::IGuestMemory& memory) { return abi::storeNew(memory, LogLevel::Warn); },
      .decode =
          [](abi::IGuestMemory& memory, abi::Address address) {
              return judge(abi::Codec<LogLevel>::load(memory, address), [](LogLevel level) {
                  return std::to_underlying(level) < abi::EnumCaseCount<LogLevel>;
              });
          } },
    { .name = "event kinds",
      .store =
          [](abi::IGuestMemory& memory) {
              return abi::storeNew(memory, EventKinds { EventKind::Title, EventKind::Font });
          },
      .decode =
          [](abi::IGuestMemory& memory, abi::Address address) {
              return judge(abi::Codec<EventKinds>::load(memory, address), [](EventKinds kinds) {
                  return (kinds.value() >> abi::FlagsMemberCount<EventKind>) == 0;
              });
          } },
    { .name = "control request",
      .store =
          [](abi::IGuestMemory& memory) {
              return abi::storeNew(memory,
                                   vtcontrol::SplitRequest { .pane = "pane:x",
                                                             .direction = vtcontrol::SplitDirection::Below,
                                                             .ratioPercent = 30,
                                                             .cwd = "/tmp",
                                                             .command = { "a", "b" },
                                                             .name = "tail",
                                                             .focus = vtcontrol::FocusPolicy::Keep });
          },
      .decode =
          [](abi::IGuestMemory& memory, abi::Address address) {
              return judge(abi::Codec<vtcontrol::SplitRequest>::load(memory, address),
                           [](vtcontrol::SplitRequest const& request) {
                               // UTF-8 throughout, and enums the vocabulary names: its JSON reads back.
                               auto const json = vtcontrol::encodeJson(request);
                               return isValidUtf8(json)
                                      && vtcontrol::decodeJson<vtcontrol::SplitRequest>(json).has_value();
                           });
          } },
});

/// A value laid out in the object: its kind, and where its root is.
struct Sample
{
    std::size_t kind;
    abi::Address address;
};

/// Lays @p memory out with values of every kind, in turn, until it is full.
/// @return Where each one is.
[[nodiscard]] std::vector<Sample> layOut(abi::IGuestMemory& memory)
{
    auto samples = std::vector<Sample> {};
    while (true)
    {
        auto const kind = samples.size() % SampleKinds.size();
        auto const stored = SampleKinds.at(kind).store(memory);
        if (!stored)
            return samples;
        samples.push_back(Sample { .kind = kind, .address = *stored });
    }
}

/// @return The count a `mutations=N` line names; nothing for another line.
[[nodiscard]] std::optional<uint64_t> mutationsIn(std::string_view line)
{
    constexpr auto Prefix = std::string_view { "mutations=" };
    if (!line.starts_with(Prefix))
        return std::nullopt;
    auto count = uint64_t { 0 };
    auto const digits = line.substr(Prefix.size());
    if (std::from_chars(digits.data(), digits.data() + digits.size(), count).ec != std::errc {})
        return std::nullopt;
    return count;
}

} // namespace

TEST_CASE("hostile memory does not break decoding", "[vtextension][hostility]")
{
    auto memory = SharedGuestMemory::create(ObjectBytes, ObjectBytes);
    REQUIRE(memory.has_value());
    auto guest = ScribbledMemory { *memory };
    auto const samples = layOut(guest);
    REQUIRE(samples.size() > SampleKinds.size());

    // The probe inherits the object as an extension host does: granted with an Instantiate.
    auto probe = process::testing::launchHelper(
        process::makeSystemProcessLauncher(),
        process::testing::HelperLaunch { .executable = VTEXTENSION_PROBE_PATH,
                                         .leading = { "--mutate", std::format("--seed={}", MutationSeed) } });
    REQUIRE(probe.has_value());
    auto control = transport::PduStream { std::move(probe->control),
                                          proto::ChannelKind::Control,
                                          proto::Direction::ToParent,
                                          proto::PduLimits {} };
    auto const target = probe->process->handleTarget();
    REQUIRE(target.has_value());
    auto const grant = transport::grantHandle(*target, memory->object());
    REQUIRE(grant.has_value());
    REQUIRE(control
                .send(proto::Instantiate { .moduleId = 1,
                                           .instanceId = 1,
                                           .interruptSlot = 0,
                                           .memory = memory->grant(grant->wireValue) },
                      *grant)
                .has_value());
    memory->closeObject();
    REQUIRE(process::testing::readReport(probe->lanes.front())
            == std::optional { std::vector<std::string> { "mutating", "end" } });

    auto outcomes = std::array<std::size_t, 3> {};
    for (auto const index: std::views::iota(0, Decodes))
    {
        auto const& sample = samples.at(static_cast<std::size_t>(index) % samples.size());
        guest.resetLiftBudget();
        auto const outcome = SampleKinds.at(sample.kind).decode(guest, sample.address);
        ++outcomes.at(std::to_underlying(outcome));
        if (outcome == Decoded::Inadmissible)
            FAIL_CHECK("an inadmissible " << SampleKinds.at(sample.kind).name << " at " << sample.address);
    }
    INFO("refused " << outcomes[std::to_underlying(Decoded::Refused)] << ", admissible "
                    << outcomes[std::to_underlying(Decoded::Admissible)]);
    CHECK(outcomes[std::to_underlying(Decoded::Inadmissible)] == 0);

    // The control channel's end ends the scribbling; the probe says how much it did, and exits.
    control.shutdown();
    auto const report = process::testing::readReport(probe->lanes.front());
    REQUIRE(report.has_value());
    REQUIRE(report->size() == 2);
    auto const mutations = mutationsIn(report->front());
    REQUIRE(mutations.has_value());
    CHECK(*mutations > 0);
    CHECK(process::testing::awaitReap(*probe->launcher, *probe->process)
          == process::ExitStatus { .kind = process::ExitKind::Exited, .value = 0 });
}
