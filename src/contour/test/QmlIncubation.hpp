// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtQml/QQmlIncubator>

#include <ranges>

namespace contour::test
{

/// Drives QML object incubation to completion.
///
/// A component built by QQmlComponent creates nested objects through the engine's incubator, and
/// asynchronously while it is itself being created. Asynchronous incubation only progresses when something
/// drives it, which a bare QQmlEngine has nothing to do; this supplies that.
///
/// A Repeater does create its delegates here, with or without a window. A delegate has a parent item but
/// no QObject parent, though, so findChild() never finds one: walk the visual tree instead
/// (SettingsPageQml_test.cpp's findDescendant). Typing into a delegate's field needs the item in an active
/// window (its TestWindow).
struct IncubationDriver: QQmlIncubationController
{
    /// Incubates until no object is left incubating, giving up after a bounded number of rounds.
    void settle()
    {
        // Bounded, so an incubation that genuinely never finishes fails the test instead of hanging it.
        for ([[maybe_unused]] auto const round: std::views::iota(0, 1000))
        {
            if (incubatingObjectCount() == 0)
                return;
            incubateFor(5);
        }
    }
};

} // namespace contour::test
