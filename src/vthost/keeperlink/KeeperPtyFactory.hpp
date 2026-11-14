// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PageSize.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/SandboxInfo.hpp>

#include <chrono>
#include <memory>

#include <vthost/keeperlink/KeeperChild.hpp>

namespace vthost::keeperlink
{

class KeeperLink;

/// Makes the devices of a daemon behind a PTY keeper: a KeeperChild per session, on a fresh PTY, spawned
/// through the keeper. One per link: it routes the keeper's Exited messages to the children it made.
class KeeperPtyFactory
{
  public:
    /// Takes over the link's child events (KeeperLink::attachChildren()): the factory and the link know each
    /// other once this returns (cyclic wiring, this one call).
    /// @param link The keeper's link (not owned; outlives this and every child made).
    /// @param placement Places each program before it runs; never null.
    /// @param hangupGrace How long the keeper gives a closed session's program before it kills it.
    KeeperPtyFactory(KeeperLink& link,
                     std::shared_ptr<vtpty::ProcessPlacement> placement,
                     std::chrono::milliseconds hangupGrace = vtpty::HangupGrace::Default);

    /// @param pageSize The terminal's size.
    /// @param exec What to run.
    /// @param sandbox Whether it escapes the sandbox this process runs in.
    /// @return The device, not yet started.
    [[nodiscard]] std::unique_ptr<KeeperChild> make(vtpty::PageSize pageSize,
                                                    vtpty::Process::ExecInfo exec,
                                                    vtpty::SandboxEscape sandbox) const;

    /// @param kept A child the keeper held while no daemon object did.
    /// @param pty The terminal it runs on, adopted.
    /// @return The device, bound to the child; its exit is heard from here on.
    [[nodiscard]] std::unique_ptr<KeeperChild> adopt(KeptChild kept, std::unique_ptr<vtpty::Pty> pty) const;

  private:
    KeeperLink& _link;
    std::shared_ptr<KeeperExits> _exits;
    std::shared_ptr<vtpty::ProcessPlacement> _placement;
    std::chrono::milliseconds _hangupGrace;
};

} // namespace vthost::keeperlink
