// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace contour
{

/// The command palette's most-recently-used list: which commands the user ran, newest first.
///
/// Pure and bounded — it holds command ids (see commandId()), never actions, so it can be persisted
/// and read back without knowing anything about what an action is. Its capacity is the number of
/// entries the palette pins above the alphabetical list.
///
/// Deliberately a plain ordered vector rather than crispy::LRUCache / StrongLRUHashtable: those are
/// hashed CACHES that map a key to a stored value and evict on pressure. What is needed here is the
/// ORDER itself — it is the thing being persisted and displayed — and at a capacity of a handful of
/// entries a linear scan is both faster and honest about what it is.
class CommandHistory
{
  public:
    /// @param capacity How many entries to remember. A capacity of 0 disables the history entirely
    ///                 (record() then does nothing and recent() stays empty).
    explicit CommandHistory(std::size_t capacity) noexcept: _capacity { capacity } {}

    /// Records that @p id was just run: moves it to the front, and drops the oldest entry when that
    /// pushes the list past its capacity.
    ///
    /// Re-running a command already in the list re-orders it rather than duplicating it, which is what
    /// makes this a most-recently-USED list and not a log of every invocation.
    ///
    /// @param id The command id that was run.
    void record(std::string_view id);

    /// Replaces the list wholesale, newest first — the load path.
    ///
    /// Entries past the capacity are dropped, so shrinking `recent_count` in the config takes effect
    /// on the next run rather than silently keeping a longer list alive in the file.
    ///
    /// @param ids The persisted ids, newest first.
    void reset(std::span<std::string const> ids);

    /// Changes how many entries to remember, dropping the oldest ones when @p capacity shrinks.
    ///
    /// Re-applied whenever the palette opens rather than fixed at construction, so editing
    /// `command_palette_recent_count` and reloading the config takes effect without a restart.
    ///
    /// @param capacity The new capacity.
    void setCapacity(std::size_t capacity);

    /// The remembered command ids, newest first.
    [[nodiscard]] std::span<std::string const> recent() const noexcept { return _recent; }

    /// How many entries this history remembers at most.
    [[nodiscard]] std::size_t capacity() const noexcept { return _capacity; }

  private:
    /// Drops whatever sits past the capacity. The one place the bound is enforced, so record(), reset()
    /// and setCapacity() cannot disagree about it.
    void trim();

    std::size_t _capacity;
    std::vector<std::string> _recent;
};

} // namespace contour
