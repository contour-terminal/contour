// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The operating system's secure keyboard entry. Qt-free: the gate that drives it is tested in contour_test.

#include <cstdint>
#include <memory>

namespace contour::platform
{

/// Whether the OS has a secure keyboard entry mode at all.
enum class SecureInputSupport : uint8_t
{
    None = 0, ///< No such mode: enable() does nothing (Linux, Windows).
    Native,   ///< The OS's own (macOS Secure Keyboard Entry).
};

/// Whether secure keyboard entry is on.
enum class SecureInputState : uint8_t
{
    Off = 0, ///< Other processes may read the keys typed into this one, as usual.
    On,      ///< No other process can read the keys typed into this one.
};

/// The OS's secure keyboard entry: while on, no other process can read the keys typed into this one.
/// Called only by SecureInputGate, which never calls enable() twice without a disable() between them.
class ISecureInput
{
  public:
    virtual ~ISecureInput() = default;

    /// Turns secure keyboard entry on.
    /// @pre GUI (main) thread; currently off.
    virtual void enable() = 0;

    /// Turns secure keyboard entry off.
    /// @pre GUI (main) thread; currently on.
    virtual void disable() = 0;

    /// @return Whether enable() does anything here.
    [[nodiscard]] virtual SecureInputSupport support() const noexcept = 0;
};

/// The secure keyboard entry of a platform that has none: every call does nothing.
class NullSecureInput final: public ISecureInput
{
  public:
    /// Does nothing.
    void enable() override {}

    /// Does nothing.
    void disable() override {}

    /// @return None.
    [[nodiscard]] SecureInputSupport support() const noexcept override { return SecureInputSupport::None; }
};

/// Creates the operating system's secure keyboard entry, or the null one where there is none. Needs no
/// QGuiApplication: the macOS one calls Carbon alone.
/// @return The secure keyboard entry, off.
[[nodiscard]] std::unique_ptr<ISecureInput> makeSecureInput();

} // namespace contour::platform
