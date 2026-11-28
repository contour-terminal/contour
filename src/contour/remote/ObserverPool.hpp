// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a window switch needs of the presence: the observers it keeps, handed over to a window landing on a
/// daemon and taken back from one leaving it. Qt-free, so that the switcher reaches the presence through it.

#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/window/DaemonSection.hpp>

#include <cstdint>
#include <memory>
#include <optional>

#include <boxed-cpp/boxed.hpp>

namespace contour::remote
{

class NativeController;

/// Whether a daemon a window leaves is one the presence wants an observer of.
enum class ObserverWanted : uint8_t
{
    No = 0, ///< Unlisted, or another window link is on it: the leaving connection ends.
    Yes,    ///< Listed, and no other window link is on it: the leaving connection may stay as its observer.
};

namespace detail::tags
{
    struct HandOffId
    {
    };
} // namespace detail::tags

/// Names one hand-over of a daemon's connection between the presence and a window's link: drawn by the
/// presence as it hands an observer to a landing link, or expects a leaving link's connection back, and named
/// again by that link's give-back. 0 names no hand-over. A link hands its daemon's connection over at most
/// once each way, so this keys the presence's marks by link without the presence knowing the link's id first.
using HandOffId = boxed::boxed<uint64_t, detail::tags::HandOffId>;

/// An observer handed over to a window landing on its daemon.
struct Adoption
{
    std::unique_ptr<NativeController>
        controller;       ///< The connection, the window's from now on; null for none.
    HandOffId handOff {}; ///< What its link's give-back names; 0 with no controller.
};

/// What a window switch needs of the presence (DaemonPresence implements it).
class IObserverPool
{
  public:
    IObserverPool() = default;
    IObserverPool(IObserverPool const&) = delete;
    IObserverPool(IObserverPool&&) = delete;
    IObserverPool& operator=(IObserverPool const&) = delete;
    IObserverPool& operator=(IObserverPool&&) = delete;
    virtual ~IObserverPool() = default;

    /// @param daemon The daemon a window lands on.
    /// @param instance The run of it the window's target names; 0 for one it does not know.
    /// @return The connected observer of @p daemon's run @p instance, marked as being handed over; none for
    ///         none.
    [[nodiscard]] virtual Adoption adoptObserver(window::DaemonRef const& daemon,
                                                 window::DaemonInstance instance) = 0;

    /// Takes a connection back, and clears the hand-off mark @p handOff names. @see DaemonPresence::giveBack
    /// @param daemon The daemon it reaches.
    /// @param handOff The hand-over it ends: the one adoptObserver() or expectGiveBack() drew; 0 for none.
    /// @param demoted The connection a window left, binding no pane; null for none.
    virtual void giveBack(window::DaemonRef const& daemon,
                          HandOffId handOff,
                          std::unique_ptr<NativeController> demoted) = 0;

    /// Marks @p daemon's connection as coming back from link @p from, so no observer is dialled meanwhile.
    /// @param daemon The daemon.
    /// @param from The link whose window leaves it.
    /// @return What the link's give-back names.
    [[nodiscard]] virtual HandOffId expectGiveBack(window::DaemonRef const& daemon, DaemonLinkId from) = 0;

    /// @param daemon The daemon a window leaves.
    /// @param leaving The link the window leaves it through.
    /// @return Yes if @p daemon stays listed, can be observed (not a `link:` daemon, which only its window
    ///         reaches) and no window link but @p leaving is on it.
    [[nodiscard]] virtual ObserverWanted wantsObserver(window::DaemonRef const& daemon,
                                                       DaemonLinkId leaving) const = 0;

    /// @param daemon A daemon.
    /// @return @p daemon's section as the sidebar lists it; none for an unlisted daemon.
    [[nodiscard]] virtual std::optional<window::DaemonSection> listedSection(
        window::DaemonRef const& daemon) const = 0;
};

} // namespace contour::remote
