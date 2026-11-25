// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The memory the extension host hands the engine for the instance it is about to instantiate: the object
/// Contour granted, mapped, and how the instance's memory asks Contour to grow.
///
/// An engine creates an instance's memory from inside its own instantiation, through a hook every instance of
/// the engine shares, where nothing names the instance. So the provision travels beside the call, on the
/// thread that makes it: the server sets it for the duration of one instantiation (ProvisionScope), and the
/// engine's hook takes it (takeProvision()), at most once.

#include <cstdint>
#include <functional>
#include <optional>

#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>

namespace vtextension::wasm::host
{

/// Asks Contour whether an instance's memory may grow to a size, and waits for the answer.
/// @param bytes The size it would grow to: whole guest pages, more than it spans now.
/// @return Whether Contour granted it.
using GrowRequest = std::function<remote::proto::GrowStatus(uint64_t bytes)>;

/// What one instance's memory is made of.
struct MemoryProvision
{
    /// The object Contour granted, mapped, with the bytes its memory spans at first open.
    remote::transport::GuestMemoryMapping memory;
    /// Asks Contour before the memory grows; nothing more is opened unless it grants.
    GrowRequest grow;
};

/// Offers a provision to the instantiation the calling thread makes while the scope lives. Whatever is not
/// taken by then is dropped, unmapping the object. Scopes nest: an inner one hides an outer one's until it
/// ends.
class ProvisionScope
{
  public:
    /// @param provision What the next memory the thread's engine creates is made of.
    explicit ProvisionScope(MemoryProvision provision);
    ~ProvisionScope();
    ProvisionScope(ProvisionScope const&) = delete;
    ProvisionScope(ProvisionScope&&) = delete;
    ProvisionScope& operator=(ProvisionScope const&) = delete;
    ProvisionScope& operator=(ProvisionScope&&) = delete;

  private:
    std::optional<MemoryProvision> _outer; ///< The provision an enclosing scope offered, put back at the end.
};

/// Takes the provision the calling thread's innermost scope offers, so that a second memory finds none.
/// @return The provision; nothing outside a scope, or once taken.
[[nodiscard]] std::optional<MemoryProvision> takeProvision() noexcept;

} // namespace vtextension::wasm::host
