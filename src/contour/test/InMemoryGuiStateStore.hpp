// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A GuiStateStore held in memory: what the Settings window and the new-defaults banner remember, without a
/// file.

#include <contour/config/GuiStateStore.hpp>

#include <expected>

namespace contour::test
{

/// Keeps the last state saved; loads it back.
class InMemoryGuiStateStore final: public config::GuiStateStore
{
  public:
    /// What was saved last; GuiState {} before the first save.
    config::GuiState state {};
    int saves = 0; ///< How many saves there were.

    [[nodiscard]] std::expected<config::GuiState, config::GuiStateError> load() const override
    {
        return state;
    }

    [[nodiscard]] std::expected<void, config::GuiStateError> save(config::GuiState const& saved) override
    {
        state = saved;
        ++saves;
        return {};
    }
};

} // namespace contour::test
