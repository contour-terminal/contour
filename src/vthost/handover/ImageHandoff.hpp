// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The last step of a live upgrade, the one that differs by platform: handing the captured daemon over to the
/// new release. POSIX execs it in this very process (ExecHandoff); Windows, which has no such exec, starts it
/// as a successor process that takes over, and ends this one (SuccessorHandoff, @see SuccessorLaunch.hpp).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/HandoverToken.hpp>
#include <vthost/handover/UpgradeError.hpp>

namespace vthost
{
class ProcessImageReplacer;
}

namespace vthost::handover
{

class HandoverMemory;

/// Hands a captured daemon over to its new release.
///
/// An interface, because the step that succeeds never returns -- this process becomes the new release, or
/// ends -- and the orchestrator is tested over a fake that throws there instead.
class ImageHandoff
{
  public:
    ImageHandoff() = default;
    ImageHandoff(ImageHandoff const&) = delete;
    ImageHandoff(ImageHandoff&&) = delete;
    ImageHandoff& operator=(ImageHandoff const&) = delete;
    ImageHandoff& operator=(ImageHandoff&&) = delete;
    virtual ~ImageHandoff() = default;

    /// Hands an image over to @p executable, which takes over as this daemon. Returns or throws only when it
    /// did not, having undone what it did; the caller then resumes the daemon.
    /// @param executable The target.
    /// @param argv The command line the target runs with, its program first; the handover argument is this
    ///        one's to append.
    /// @param crossing Every descriptor or handle the image names (handlesNamedBy()): what the target keeps
    ///        across the exec, or inherits.
    /// @param bytes The image, encoded. Taken, so that a handoff can let go of it once the memory object
    ///        holds it, rather than keep it through the wait for the target.
    /// @return Why the target did not take over.
    [[nodiscard]] virtual ExecFailure handOver(std::filesystem::path const& executable,
                                               std::vector<std::string> argv,
                                               std::vector<vtpty::CrossingHandle> crossing,
                                               std::vector<uint8_t> bytes) = 0;
};

/// The POSIX handoff: keeps every descriptor the image names across the exec, writes the image into an
/// anonymous memory object, and execs the target in this process with `--handover=fd=N`, which keeps every
/// hosted program's parent, process group and controlling terminal.
class ExecHandoff final: public ImageHandoff
{
  public:
    /// @param replacer What execs the target. Not owned.
    /// @param memory Where the image is written, and what keeps descriptors across the exec. Not owned.
    ExecHandoff(ProcessImageReplacer& replacer, HandoverMemory& memory) noexcept:
        _replacer { replacer }, _memory { memory }
    {
    }

    [[nodiscard]] ExecFailure handOver(std::filesystem::path const& executable,
                                       std::vector<std::string> argv,
                                       std::vector<vtpty::CrossingHandle> crossing,
                                       std::vector<uint8_t> bytes) override;

  private:
    ProcessImageReplacer& _replacer;
    HandoverMemory& _memory;
};

} // namespace vthost::handover
