// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/ProgramStatus.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
    /// @return The id updated, and the id evicted when the set was full.
    [[nodiscard]] ProgramStatusChange apply(ProgramStatusRecord record);

    /// Removes the record @p id names and every record beneath it.
    /// @param id The subtree's root; empty removes every record.
    [[nodiscard]] ProgramStatusChange clear(std::string_view id);

    /// A new shell prompt: drops the working and blocked records, keeps idle, done and error.
    [[nodiscard]] ProgramStatusChange promptStarted();

    /// The terminal's process exited: the same drops as promptStarted().
    [[nodiscard]] ProgramStatusChange processExited();

    /// RIS: removes every record.
    [[nodiscard]] ProgramStatusChange reset();

    /// Replaces the whole set with a mirrored host's, keeping at most the cap (the newest).
    /// @param records The host's records, least recently updated first.
    /// @return Every id that is new or differs, and every id that is gone; reason Mirror.
    [[nodiscard]] ProgramStatusChange replaceAll(std::vector<ProgramStatusRecord> records);

    /// @return The records, least recently updated first.
    [[nodiscard]] std::span<ProgramStatusRecord const> records() const noexcept { return _records; }

    /// @return The record of @p id, or null.
    [[nodiscard]] ProgramStatusRecord const* find(std::string_view id) const noexcept;

    /// The program a record speaks for: its own `app`, else that of its nearest ancestor that has one.
    /// @param record A record of this set.
    /// @return The app, or an empty view when neither it nor an ancestor names one.
    [[nodiscard]] std::string_view appOf(ProgramStatusRecord const& record) const noexcept;

    /// @return The most records kept.
    [[nodiscard]] std::size_t maxRecords() const noexcept { return _maxRecords; }

  private:
    /// Removes every record @p drops selects.
    template <typename Predicate>
    [[nodiscard]] ProgramStatusChange removeIf(Predicate drops, ProgramStatusReason reason);

    std::size_t _maxRecords;
    std::vector<ProgramStatusRecord> _records; ///< Least recently updated first.
};

} // namespace vtbackend
