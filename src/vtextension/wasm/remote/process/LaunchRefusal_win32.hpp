// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Windows only: where the system refused to start a process, and with which code. A LaunchError says what a
/// refusal means; for the ones that cannot say what was refused (DetailedLaunchErrors), the launcher logs
/// this too, so that a refusal seen once -- on a user's machine, on a CI runner -- can be traced.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::process
{

/// A step of starting a process that the system may refuse.
enum class LaunchStep : uint8_t
{
    Executable,         ///< Looking the executable up.
    Profile,            ///< Creating or finding the AppContainer profile.
    ContainerFolder,    ///< Asking for the AppContainer's own folder.
    PackagesSid,        ///< Spelling the SID of ALL APPLICATION PACKAGES.
    ReadGrant,          ///< Reading the executable directory's DACL.
    Grant,              ///< Adding the AppContainer grant to it.
    WriteGrant,         ///< Writing it back.
    CopyHandle,         ///< Duplicating a handle the child inherits.
    NullDevice,         ///< Opening the null device.
    Job,                ///< Making the child's job.
    AttributeList,      ///< Making the attribute list.
    HandleList,         ///< Setting the attribute of the handles it inherits.
    JobList,            ///< Setting the attribute of its job.
    Container,          ///< Setting the attribute of its AppContainer.
    ChildProcessPolicy, ///< Setting the attribute that forbids it a child process.
    Spelling,           ///< Spelling its command line or environment in UTF-16.
    Inheritance,        ///< Making its handles inheritable.
    CreateProcess,      ///< Creating the process.
};

/// How a step's code is to be read.
enum class RefusalCode : uint8_t
{
    Win32,   ///< A Win32 error code (GetLastError()), said in decimal.
    HResult, ///< An HRESULT, said in hexadecimal.
};

/// One step: what it is, in a line of Contour's log, and how its code reads.
struct LaunchStepRow
{
    LaunchStep step;       ///< The step.
    std::string_view text; ///< What Windows refused, completing "Windows refused ...".
    RefusalCode code;      ///< How its code reads.
};

/// Every LaunchStep, one row each, at its enumerator's index.
inline constexpr auto LaunchSteps = std::to_array<LaunchStepRow>({
    { .step = LaunchStep::Executable, .text = "to find its executable", .code = RefusalCode::Win32 },
    { .step = LaunchStep::Profile,
      .text = "to create or find its app container profile",
      .code = RefusalCode::HResult },
    { .step = LaunchStep::ContainerFolder,
      .text = "to name its app container's folder",
      .code = RefusalCode::HResult },
    { .step = LaunchStep::PackagesSid,
      .text = "to name ALL APPLICATION PACKAGES",
      .code = RefusalCode::Win32 },
    { .step = LaunchStep::ReadGrant,
      .text = "to read the access list of its directory",
      .code = RefusalCode::Win32 },
    { .step = LaunchStep::Grant,
      .text = "to add app containers to the access list of its directory",
      .code = RefusalCode::Win32 },
    { .step = LaunchStep::WriteGrant,
      .text = "to write the access list of its directory",
      .code = RefusalCode::Win32 },
    { .step = LaunchStep::CopyHandle, .text = "to copy a handle it inherits", .code = RefusalCode::Win32 },
    { .step = LaunchStep::NullDevice, .text = "to open the null device", .code = RefusalCode::Win32 },
    { .step = LaunchStep::Job, .text = "to make its job", .code = RefusalCode::Win32 },
    { .step = LaunchStep::AttributeList, .text = "to make its attribute list", .code = RefusalCode::Win32 },
    { .step = LaunchStep::HandleList, .text = "the handles it inherits", .code = RefusalCode::Win32 },
    { .step = LaunchStep::JobList, .text = "to start it in its job", .code = RefusalCode::Win32 },
    { .step = LaunchStep::Container, .text = "to start it in its app container", .code = RefusalCode::Win32 },
    { .step = LaunchStep::ChildProcessPolicy,
      .text = "to forbid it child processes",
      .code = RefusalCode::Win32 },
    { .step = LaunchStep::Spelling,
      .text = "to spell its command line or environment",
      .code = RefusalCode::Win32 },
    { .step = LaunchStep::Inheritance, .text = "to let it inherit its handles", .code = RefusalCode::Win32 },
    { .step = LaunchStep::CreateProcess, .text = "to create its process", .code = RefusalCode::Win32 },
});

/// The launch errors whose text cannot say what the system refused: a refusal mapped to one of them is
/// logged with its step and code. `NotFound`, `Resources` and `NotGranted` say enough; `Stopped` is no
/// refusal.
inline constexpr auto DetailedLaunchErrors = std::to_array<LaunchError>({
    LaunchError::Unsupported,
    LaunchError::Io,
});

/// Where and how the system refused a launch.
struct LaunchRefusal
{
    LaunchError error; ///< What it means to the launch.
    LaunchStep step;   ///< The step refused.
    /// The system's code: GetLastError()'s, or the HRESULT where the step's row says so. An HRESULT is
    /// carried bit for bit.
    DWORD code;

    bool operator==(LaunchRefusal const&) const = default;
};

/// @param refusal Where and how the system refused a launch.
/// @return Its line in Contour's log: the step and the code, with the system's own words for it where it
///         has some, as UTF-8.
[[nodiscard]] std::string launchRefusalLine(LaunchRefusal const& refusal);

} // namespace vtextension::wasm::remote::process
