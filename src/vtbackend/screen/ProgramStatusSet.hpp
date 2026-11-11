// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/ProgramStatus.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

/// Why a terminal's program-status records changed.
enum class ProgramStatusReason : uint8_t
{
    Report = 0,  ///< An OSC 7501 report set or cleared records.
    Prompt,      ///< A new shell prompt dropped the working and blocked records.
    ProcessExit, ///< The terminal's process exited, dropping the working and blocked records.
    Reset,       ///< RIS removed every record.
    Mirror,      ///< A daemon mirror replaced the set with the host's.
    UserInput,   ///< The user typed, so an integration's blocked records are working again.
};

/// How many ProgramStatusReason enumerators there are: what a table of them must cover.
inline constexpr auto ProgramStatusReasonCount =
    std::size_t { std::to_underlying(ProgramStatusReason::UserInput) + 1 };

/// Whether mirrored records carry the host's `since` and generation.
enum class ProgramStatusTiming : uint8_t
{
    Absent = 0, ///< They carry none (a host without the timing capability): this set stamps them.
    Carried,    ///< They carry the host's, which are kept as they are.
};

/// What one change to a ProgramStatusSet did, by record id.
struct ProgramStatusChange
{
    std::vector<std::string> updated; ///< Ids whose record was set (new or replaced).
    std::vector<std::string> removed; ///< Ids whose record is gone (cleared, dropped or evicted).
    ProgramStatusReason reason = ProgramStatusReason::Report; ///< What caused it.

    /// @return Whether the change did nothing, so that nobody need hear of it.
    [[nodiscard]] bool empty() const noexcept { return updated.empty() && removed.empty(); }
};

/// The program-status records of one terminal, and the rules of their lifetime (program-status rev 0.4).
///
/// Records belong to the terminal, not to a screen: an alternate-screen switch and DECSTR leave them
/// alone. A report replaces its id's record whole. At the cap the least recently updated record is
/// evicted, so a loop reporting fresh ids cannot grow the set.
///
/// Not thread-safe: Terminal owns one and touches it under its state lock.
class ProgramStatusSet
{
  public:
    /// @param maxRecords The most records kept; the spec's minimum is the default.
    explicit ProgramStatusSet(std::size_t maxRecords = MaxProgramStatusRecords);

    /// Sets @p record, replacing the record of its id whole.
    ///
    /// The stored record's `since` and `generation` are the set's to assign: a new record, or one whose
    /// state or kind differs from the record it replaces, gets @p now and a fresh generation; any other
    /// report keeps the replaced record's `since` and `generation`.
    /// @param record The record to set; its `since` and `generation` are overwritten.
    /// @param now The terminal's wall clock.
    /// @return The id updated, and the id evicted when the set was full.
    [[nodiscard]] ProgramStatusChange apply(ProgramStatusRecord record,
                                            std::chrono::system_clock::time_point now);

    /// The user typed: every blocked integration record becomes working (kind None), with a fresh
    /// generation and `since` of @p now. Program-origin records are left alone.
    /// @param now The terminal's wall clock.
    /// @return The ids changed; reason UserInput. Empty when nothing matched.
    [[nodiscard]] ProgramStatusChange noteUserInput(std::chrono::system_clock::time_point now);

    /// Removes the record @p id names and every record beneath it.
    /// @param id The subtree's root; empty removes every record.
    [[nodiscard]] ProgramStatusChange clear(std::string_view id);

    /// Removes the integration's own record @p id names, and nothing beneath it or of another origin.
    /// @param id The record's id.
    /// @return The id removed; empty when no integration record has that id.
    [[nodiscard]] ProgramStatusChange clearIntegration(std::string_view id);

    /// A new shell prompt: drops the working and blocked records and every integration record, keeps the
    /// program's idle, done and error.
    [[nodiscard]] ProgramStatusChange promptStarted();

    /// The terminal's process exited: the same drops as promptStarted().
    [[nodiscard]] ProgramStatusChange processExited();

    /// RIS: removes every record.
    [[nodiscard]] ProgramStatusChange reset();

    /// Replaces the whole set with a mirrored host's, keeping at most the cap (the newest).
    /// @param records The host's records, least recently updated first.
    /// @param timing  Carried: their `since` and `generation` are kept verbatim, and nextGeneration() rises
    ///                above the highest of them. Absent: each is stamped as apply() stamps one -- a record
    ///                whose state and kind did not change keeps the instant and generation it had here, any
    ///                other takes @p now and the next generation -- so a Done from an older host still reads
    ///                as unseen.
    /// @param now     The instant an Absent record that changed is stamped with.
    /// @return Every id that is new or differs, and every id that is gone; reason Mirror.
    [[nodiscard]] ProgramStatusChange replaceAll(std::vector<ProgramStatusRecord> records,
                                                 ProgramStatusTiming timing,
                                                 std::chrono::system_clock::time_point now);

    /// @return The records, least recently updated first.
    [[nodiscard]] std::span<ProgramStatusRecord const> records() const noexcept { return _records; }

    /// @return The record of @p id, or null.
    [[nodiscard]] ProgramStatusRecord const* find(std::string_view id) const noexcept;

    /// The program a record speaks for: its own `app`, else that of its nearest ancestor that has one.
    /// @param record A record of this set.
    /// @return The app, or an empty view when neither it nor an ancestor names one.
    [[nodiscard]] std::string_view appOf(ProgramStatusRecord const& record) const noexcept;

    /// @return The generation the next new or changed record will get.
    [[nodiscard]] uint64_t nextGeneration() const noexcept { return _nextGeneration; }

    /// Raises the generation counter, as a restored terminal state does; never lowers it.
    /// @param next The generation the next new or changed record should get at least.
    void restoreNextGeneration(uint64_t next) noexcept { _nextGeneration = std::max(_nextGeneration, next); }

    /// @return The most records kept.
    [[nodiscard]] std::size_t maxRecords() const noexcept { return _maxRecords; }

  private:
    /// Removes every record @p drops selects.
    template <typename Predicate>
    [[nodiscard]] ProgramStatusChange removeIf(Predicate drops, ProgramStatusReason reason);

    /// Stamps @p record as the set's record of the same id would leave it: that one's since and generation
    /// when the state and kind are unchanged, else @p now and a new generation.
    void stamp(ProgramStatusRecord& record, std::chrono::system_clock::time_point now);

    std::size_t _maxRecords;
    uint64_t _nextGeneration = 1; ///< Handed to the next new or changed record; 0 means unassigned.
    std::vector<ProgramStatusRecord> _records; ///< Least recently updated first.
};

} // namespace vtbackend
