// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/WorkspaceListing.hpp>
#include <contour/platform/OffThreadJobs.hpp>
#include <contour/remote/WorkspaceLister.hpp>

#include <core/async/StopToken.hpp>

#include <memory>
#include <stop_token>
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
            // Stopped as this lister goes: the dial, a question about the daemon and the call all end then,
            // rather than hold the owner's destruction up, or outlive what they borrow.
            _jobs.run(
                [timeout = _timeout, transport = std::move(target.transport)](std::stop_token const& stop) {
                    auto source = core::async::StopSource {};
                    auto const forward = std::stop_callback { stop, [&source] { source.request_stop(); } };
                    auto listed = WorkspaceListing {};
                    if (auto const answer = vthost::client::runControlCall(
                            *transport, "workspace.list", "{}", "", timeout, source.get_token());
                        !answer)
                        listed = std::unexpected(ListFailure {
                            .kind = answer.error().failure == vthost::client::ControlFailure::Declined
                                        ? ListFailureKind::Declined
                                        : ListFailureKind::Failed,
                            .reason = answer.error().message });
                    else
                        listed = cli::decodeWorkspaceList(*answer).transform_error(
                            [](std::string reason) { return ListFailure { .reason = std::move(reason) }; });
                    return listed;
                },
                [done = std::move(done)](auto listed) mutable { done(std::move(listed)); });
        }

        void create(ListTarget target, std::string label, WorkspaceCreated done) override
        {
            _jobs.run(
                [timeout = _timeout, transport = std::move(target.transport), label = std::move(label)] {
                    auto const answer = vthost::client::runControlCall(
                        *transport, "workspace.create", cli::encodeCreateWorkspace(label), "", timeout);
                    if (!answer)
                        return std::expected<vtcontrol::ObjectRef, std::string> { std::unexpected(
                            answer.error().message) };
                    return cli::decodeCreatedWorkspace(*answer);
                },
                [done = std::move(done)](auto created) mutable { done(std::move(created)); });
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
