// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/WorkspaceListing.hpp>
#include <contour/platform/OffThreadJobs.hpp>
#include <contour/remote/WorkspaceLister.hpp>

#include <memory>
#include <utility>

#include <vthost/client/ControlClient.hpp>

namespace contour::remote
{

namespace
{
    /// `workspace.list` over the control API, each listing on a thread of its own.
    class ControlWorkspaceLister final: public IWorkspaceLister
    {
      public:
        explicit ControlWorkspaceLister(std::chrono::milliseconds timeout): _timeout(timeout) {}
        ControlWorkspaceLister(ControlWorkspaceLister const&) = delete;
        ControlWorkspaceLister(ControlWorkspaceLister&&) = delete;
        ControlWorkspaceLister& operator=(ControlWorkspaceLister const&) = delete;
        ControlWorkspaceLister& operator=(ControlWorkspaceLister&&) = delete;
        ~ControlWorkspaceLister() override = default;

        void list(ListTarget target, WorkspacesListed done) override
        {
            // Off the GUI thread: an ssh connect takes seconds. The answer is told back here.
            _jobs.run(
                [timeout = _timeout, transport = std::move(target.transport)] {
                    auto listed = std::expected<std::vector<vtcontrol::WorkspaceInfo>, std::string> {};
                    if (auto const answer =
                            vthost::client::runControlCall(*transport, "workspace.list", "{}", "", timeout);
                        !answer)
                        listed = std::unexpected(answer.error().message);
                    else
                        listed = cli::decodeWorkspaceList(*answer);
                    return listed;
                },
                [done = std::move(done)](auto listed) mutable { done(std::move(listed)); });
        }

      private:
        std::chrono::milliseconds _timeout;
        /// The listings under way; the ones that ended are joined as the next starts, which the switch
        /// directory does on every cycling press.
        platform::OffThreadJobs _jobs;
    };
} // namespace

std::unique_ptr<IWorkspaceLister> makeControlWorkspaceLister(std::chrono::milliseconds timeout)
{
    return std::make_unique<ControlWorkspaceLister>(timeout);
}

} // namespace contour::remote
