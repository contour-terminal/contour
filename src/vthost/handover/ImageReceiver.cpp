// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/ImageReceiver.hpp>

#include <vtpty/CrossingHandle.hpp>

#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/handover/HandoverToken.hpp>

#ifdef _WIN32
    #include <vthost/handover/SuccessorLaunch.hpp>
#endif

namespace vthost::handover
{

namespace
{
#ifdef _WIN32
    /// The successor's receiver: `handles=M,R,C,P`, then receiveHandover().
    class SuccessorReceiver final: public ImageReceiver
    {
      public:
        explicit SuccessorReceiver(HandoverMemory& memory) noexcept: _memory { memory } {}

        [[nodiscard]] std::expected<std::optional<HandoverImage>, TakeoverFailure> takeOver(
            std::string_view token) override
        {
            if (token.empty())
                return std::nullopt;
            auto const handles = parseHandoverHandles(token);
            if (!handles)
            {
                errorLog()("--handover={} names no handles of a successor; ending without serving", token);
                return std::unexpected(TakeoverFailure::EndWithoutServing);
            }
            // Ends without serving where it may take nothing over: the predecessor, which never saw this
            // one ready, ends it and serves on.
            return receiveHandover(*handles, _memory)
                .transform([](HandoverImage image) { return std::optional { std::move(image) }; })
                .transform_error([](std::string const& why) {
                    errorLog()("{}; ending without serving", why);
                    return TakeoverFailure::EndWithoutServing;
                });
        }

      private:
        HandoverMemory& _memory;
    };
#else
    /// The exec'd image's receiver: `fd=N`, then the image in that memory object.
    class ExecReceiver final: public ImageReceiver
    {
      public:
        explicit ExecReceiver(HandoverMemory& memory) noexcept: _memory { memory } {}

        [[nodiscard]] std::expected<std::optional<HandoverImage>, TakeoverFailure> takeOver(
            std::string_view token) override
        {
            if (token.empty())
                return std::nullopt;
            // A token that names no descriptor names nothing to inherit: nothing is closed.
            auto const fd = parseHandoverDescriptor(token);
            if (!fd)
            {
                errorLog()("--handover={} names no descriptor; starting afresh", token);
                return std::unexpected(TakeoverFailure::StartAfresh);
            }
            auto image = readHandover(_memory, *fd);
            if (!image)
            {
                // Inherited, but no longer known by name: the listeners the daemon binds anew would find
                // them taken. Every other descriptor stays open, so no hosted program is hung up on; they
                // run on unreachable until they end, having no session here to be shown in.
                auto const closed = closeListeningSockets();
                errorLog()("cannot read the image the previous daemon handed over (error {}); starting "
                           "afresh. {} inherited listener(s) closed; the programs it hosted keep running, "
                           "unreachable",
                           std::to_underlying(image.error()),
                           closed);
                return std::unexpected(TakeoverFailure::StartAfresh);
            }
            // Whatever the image does not name was leaked across the exec.
            closeDescriptorsExcept(descriptorsToKeep(*image));
            return std::optional { *std::move(image) };
        }

      private:
        HandoverMemory& _memory;
    };
#endif
} // namespace

std::unique_ptr<ImageReceiver> makeNativeImageReceiver(HandoverMemory& memory)
{
#ifdef _WIN32
    return std::make_unique<SuccessorReceiver>(memory);
#else
    return std::make_unique<ExecReceiver>(memory);
#endif
}

} // namespace vthost::handover
