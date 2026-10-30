// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A recording double for the OS's secure keyboard entry. Qt-free, so contour_test can use it.

#include <contour/platform/SecureInput.hpp>

namespace contour::test
{

/// What a RecordingSecureInput was asked, and what it answers. Owned by the test, so the counts stay
/// readable after whatever owned the RecordingSecureInput -- an app, a manager -- is gone.
struct SecureInputRecord
{
    int enables = 0;  ///< How often enable() was called.
    int disables = 0; ///< How often disable() was called.
    platform::SecureInputSupport supported =
        platform::SecureInputSupport::Native; ///< What support() answers.
};

/// A secure keyboard entry that counts into a SecureInputRecord instead of reaching the OS.
class RecordingSecureInput final: public platform::ISecureInput
{
  public:
    /// @param record Where to count; it must outlive this object.
    explicit RecordingSecureInput(SecureInputRecord& record): _record { record } {}

    /// Counts the call.
    void enable() override { ++_record.enables; }

    /// Counts the call.
    void disable() override { ++_record.disables; }

    /// @return The record's answer.
    [[nodiscard]] platform::SecureInputSupport support() const noexcept override { return _record.supported; }

  private:
    SecureInputRecord& _record;
};

} // namespace contour::test
