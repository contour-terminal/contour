// SPDX-License-Identifier: Apache-2.0
//
// The macOS chain verifier. Every certificate the server presented goes into one SecTrust with the
// SSL server policy for the name, and `SecTrustEvaluateWithError` decides -- the keychains' trust
// settings, a root Apple distrusts -- on the blocking executor, because it may block.
//
// It never fetches what the server names: `SecTrustSetNetworkFetchAllowed(false)` keeps the
// evaluation from following a certificate's Authority Information Access URL to an intermediate, a
// request to a host of the server's choosing that no policy of the caller's saw. A server that sends
// an incomplete chain is refused here, as OpenSSL refuses it on every other system.
#include "DarwinChainVerifier.hpp"

#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/Tls.hpp>

#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace core::net::detail
{

namespace
{
    /// Releases a Core Foundation reference; tolerates null.
    struct CfRelease
    {
        void operator()(void const* reference) const noexcept
        {
            if (reference != nullptr)
                CFRelease(reference);
        }
    };

    /// Owns a Core Foundation reference of type @p Ref (a pointer typedef such as `CFDataRef`).
    template <typename Ref>
    using CfPtr = std::unique_ptr<std::remove_pointer_t<Ref>, CfRelease>;

    /// A refusal, for any result type that carries a @c TlsFailure.
    [[nodiscard]] std::unexpected<TlsFailure> refusal(std::string message)
    {
        return std::unexpected(TlsFailure {
            .kind = TlsFailureKind::Certificate, .alert = std::nullopt, .message = std::move(message) });
    }

    /// @p text as UTF-8; empty where it cannot be converted.
    [[nodiscard]] std::string utf8Of(CFStringRef text)
    {
        if (text == nullptr)
            return {};
        auto const capacity =
            CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
        auto buffer = std::string(static_cast<std::size_t>(capacity), '\0');
        if (CFStringGetCString(text, buffer.data(), capacity, kCFStringEncodingUTF8) == 0)
            return {};
        buffer.resize(std::strlen(buffer.c_str()));
        return buffer;
    }

    /// What `SecTrustEvaluateWithError` said was wrong, for the refusal's message.
    [[nodiscard]] std::string describe(CFErrorRef error)
    {
        auto const description =
            CfPtr<CFStringRef> { error != nullptr ? CFErrorCopyDescription(error) : nullptr };
        auto text = utf8Of(description.get());
        return text.empty() ? std::string { "the platform's trust evaluation refused the chain" } : text;
    }

    /// Builds the trust object @p chain is evaluated through, for @p serverName with the SSL server
    /// policy, network fetching off.
    [[nodiscard]] std::expected<CfPtr<SecTrustRef>, TlsFailure> trustFor(
        std::vector<DerCertificate> const& chain, std::string const& serverName)
    {
        if (chain.empty())
            return refusal("the peer presented no certificate");
        auto const certificates = CfPtr<CFMutableArrayRef> { CFArrayCreateMutable(
            kCFAllocatorDefault, static_cast<CFIndex>(chain.size()), &kCFTypeArrayCallBacks) };
        if (!certificates)
            return refusal("CFArrayCreateMutable failed");
        for (auto const& der: chain)
        {
            auto const data = CfPtr<CFDataRef> { CFDataCreate(kCFAllocatorDefault,
                                                              reinterpret_cast<UInt8 const*>(der.data()),
                                                              static_cast<CFIndex>(der.size())) };
            if (!data)
                return refusal("CFDataCreate failed");
            auto const certificate =
                CfPtr<SecCertificateRef> { SecCertificateCreateWithData(kCFAllocatorDefault, data.get()) };
            if (!certificate)
                return refusal("a certificate in the presented chain does not parse");
            CFArrayAppendValue(certificates.get(), certificate.get());
        }

        // An empty name checks the chain alone, as SecPolicyCreateSSL documents for a null hostname.
        auto const name = CfPtr<CFStringRef> {
            serverName.empty() ? nullptr
                               : CFStringCreateWithBytes(kCFAllocatorDefault,
                                                         reinterpret_cast<UInt8 const*>(serverName.data()),
                                                         static_cast<CFIndex>(serverName.size()),
                                                         kCFStringEncodingUTF8,
                                                         static_cast<Boolean>(false))
        };
        if (!serverName.empty() && !name)
            return refusal("the server name is not UTF-8");
        auto const policy =
            CfPtr<SecPolicyRef> { SecPolicyCreateSSL(static_cast<Boolean>(true), name.get()) };
        if (!policy)
            return refusal("SecPolicyCreateSSL failed");

        auto rawTrust = SecTrustRef { nullptr };
        if (auto const status = SecTrustCreateWithCertificates(certificates.get(), policy.get(), &rawTrust);
            status != errSecSuccess)
            return refusal("SecTrustCreateWithCertificates: OSStatus " + std::to_string(status));
        auto trust = CfPtr<SecTrustRef> { rawTrust };
        if (auto const status = SecTrustSetNetworkFetchAllowed(trust.get(), static_cast<Boolean>(false));
            status != errSecSuccess)
            return refusal("SecTrustSetNetworkFetchAllowed: OSStatus " + std::to_string(status));
        return trust;
    }

    /// Evaluates @p chain for @p serverName with the SSL server policy. Blocking.
    [[nodiscard]] std::expected<void, TlsFailure> evaluate(std::vector<DerCertificate> const& chain,
                                                           std::string const& serverName)
    {
        auto const trust = trustFor(chain, serverName);
        if (!trust)
            return std::unexpected(trust.error());
        auto rawError = CFErrorRef { nullptr };
        auto const trusted = SecTrustEvaluateWithError(trust->get(), &rawError);
        auto const error = CfPtr<CFErrorRef> { rawError };
        if (!trusted)
            return refusal(describe(error.get()));
        return {};
    }

    class DarwinChainVerifier final: public IChainVerifier
    {
      public:
        explicit DarwinChainVerifier(async::IExecutor& blocking) noexcept: _blocking { &blocking } {}

        [[nodiscard]] async::Task<std::expected<void, TlsFailure>> verify(std::vector<DerCertificate> chain,
                                                                          std::string serverName) override
        {
            co_await async::ResumeOn { *_blocking };
            co_return evaluate(chain, serverName);
        }

      private:
        async::IExecutor* _blocking;
    };
} // namespace

std::expected<bool, std::string> networkFetchAllowed(std::vector<DerCertificate> const& chain,
                                                     std::string const& serverName)
{
    auto const trust = trustFor(chain, serverName);
    if (!trust)
        return std::unexpected(trust.error().message);
    auto allowed = Boolean {};
    if (auto const status = SecTrustGetNetworkFetchAllowed(trust->get(), &allowed); status != errSecSuccess)
        return std::unexpected("SecTrustGetNetworkFetchAllowed: OSStatus " + std::to_string(status));
    return allowed != 0;
}

std::shared_ptr<IChainVerifier> makeDarwinChainVerifier(async::IExecutor& blocking)
{
    return std::make_shared<DarwinChainVerifier>(blocking);
}

} // namespace core::net::detail
