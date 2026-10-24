// SPDX-License-Identifier: Apache-2.0
#include <vtcontrol/ControlApi.hpp>

#include <vtcontrol/Describe.hpp>

namespace vtcontrol
{

namespace
{
    constexpr auto NotServedMessage = std::string_view { "the control API is not served here" };

    template <typename Reply>
    void refuse(Completion<Reply>& done)
    {
        done(std::unexpected(ControlError { .code = ControlErrorCode::Unsupported,
                                            .message = std::string { NotServedMessage },
                                            .candidates = {} }));
    }

    /// Serves nothing.
    class NullControlApi final: public ControlApi
    {
      public:
        // clang-format off
        void handle(CallContext& /*context*/, DaemonInfoRequest const& /*request*/, Completion<DaemonInfoReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, ListRequest const& /*request*/, Completion<ListReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, CreateTabRequest const& /*request*/, Completion<CreateTabReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, SplitRequest const& /*request*/, Completion<SplitReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, SendRequest const& /*request*/, Completion<SendReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, CaptureRequest const& /*request*/, Completion<CaptureReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, ClosePaneRequest const& /*request*/, Completion<EmptyReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, CloseTabRequest const& /*request*/, Completion<EmptyReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, SetLabelRequest const& /*request*/, Completion<EmptyReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, BlockListRequest const& /*request*/, Completion<BlockListReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, BlockReadRequest const& /*request*/, Completion<BlockReadReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, WaitRequest const& /*request*/, Completion<WaitReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, SubscribeRequest const& /*request*/, Completion<SubscribeReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, UnsubscribeRequest const& /*request*/, Completion<EmptyReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, ApiListRequest const& /*request*/, Completion<ApiListReply> done) override { refuse(done); }
        void handle(CallContext& /*context*/, ApiDescribeRequest const& /*request*/, Completion<ApiDescription> done) override { refuse(done); }
        // clang-format on
    };
} // namespace

void ControlApi::handle(CallContext& /*context*/,
                        ApiListRequest const& /*request*/,
                        Completion<ApiListReply> done)
{
    done(listApi());
}

void ControlApi::handle(CallContext& /*context*/,
                        ApiDescribeRequest const& request,
                        Completion<ApiDescription> done)
{
    done(describeApi(request.name));
}

void ControlApi::clientAttached(std::string_view /*client*/)
{
}

void ControlApi::clientDetached(std::string_view /*client*/)
{
}

void ControlApi::sinkClosed(EventSink& /*sink*/)
{
}

ControlApi& nullControlApi() noexcept
{
    static auto instance = NullControlApi {};
    return instance;
}

} // namespace vtcontrol
