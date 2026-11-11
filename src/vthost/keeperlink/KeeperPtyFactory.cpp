// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/KeeperPtyFactory.hpp>

#include <core/Assert.hpp>

#include <utility>

#include <vthost/keeperlink/KeeperLink.hpp>

namespace vthost::keeperlink
{

KeeperPtyFactory::KeeperPtyFactory(KeeperLink& link,
                                   std::shared_ptr<vtpty::ProcessPlacement> placement,
                                   std::chrono::milliseconds hangupGrace):
    _link { link },
    _exits { std::make_shared<KeeperExits>() },
    _placement { std::move(placement) },
    _hangupGrace { hangupGrace }
{
    Require(_placement != nullptr);
    _link.onExited([exits = _exits](keeper::KeeperSessionId session, vtpty::ExitStatus status) {
        exits->exited(session, status);
    });
}

std::unique_ptr<KeeperChild> KeeperPtyFactory::make(vtpty::PageSize pageSize,
                                                    vtpty::Process::ExecInfo exec,
                                                    vtpty::SandboxEscape sandbox) const
{
    return std::make_unique<KeeperChild>(_link,
                                         _exits,
                                         KeeperChildConfig { .exec = std::move(exec),
                                                             .sandbox = sandbox,
                                                             .placement = _placement,
                                                             .hangupGrace = _hangupGrace },
                                         vtpty::createPty(pageSize, std::nullopt));
}

} // namespace vthost::keeperlink
