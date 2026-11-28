// SPDX-License-Identifier: Apache-2.0
//
// The Windows chain verifier. The certificates the server presented go into an in-memory store, the
// chain engine builds the leaf's chain from it and the system stores (`CertGetCertificateChain`,
// for server authentication), and the SSL policy checks it for the name
// (`CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL)`). On the blocking executor, because
// the engine may fetch a root through Automatic Root Update.
//
// It never fetches what the server names: `CERT_CHAIN_DISABLE_AIA` keeps the engine from following
// a certificate's Authority Information Access URL to an intermediate, a request to a host of the
// server's choosing that no policy of the caller's saw. A server that sends an incomplete chain is
// refused here, as OpenSSL refuses it on every other system.
//
// No revocation check is requested: an offline responder would then refuse every connection, and
// the soft-fail that avoids it would accept a revoked certificate whenever the answer is blocked.
#include "WindowsChainVerifier.hpp"

#include <core/WideString.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/ITlsSession.hpp>
#include <core/net/Tls.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

// After <windows.h>, which declares the types it uses; every core-cpp target builds with
// WIN32_LEAN_AND_MEAN, under which <windows.h> does not include it.
#include <wincrypt.h>

namespace core::net::detail
{

namespace
{
    /// `SECURITY_FLAG_IGNORE_CERT_CN_INVALID`, from `<wininet.h>`, which is not included for one
    /// constant: the SSL policy checks no name.
    constexpr auto IgnoreServerName = DWORD { 0x00001000 };

    struct StoreClose
    {
        void operator()(void* store) const noexcept { CertCloseStore(store, 0); }
    };
    using StorePtr = std::unique_ptr<void, StoreClose>;

    struct ContextFree
    {
        void operator()(CERT_CONTEXT const* context) const noexcept { CertFreeCertificateContext(context); }
    };
    using CertificatePtr = std::unique_ptr<CERT_CONTEXT const, ContextFree>;

    struct ChainFree
    {
        void operator()(CERT_CHAIN_CONTEXT const* chain) const noexcept { CertFreeCertificateChain(chain); }
    };
    using ChainPtr = std::unique_ptr<CERT_CHAIN_CONTEXT const, ChainFree>;

    [[nodiscard]] std::expected<void, TlsFailure> refusal(std::string message)
    {
        return std::unexpected(TlsFailure {
            .kind = TlsFailureKind::Certificate, .alert = std::nullopt, .message = std::move(message) });
    }

    /// A Win32 error or an HRESULT as `0x800B0109: <what Windows calls it>`.
    [[nodiscard]] std::string describe(std::string_view call, DWORD code)
    {
        return std::format("{}: 0x{:08X}: {}", call, code, core::systemErrorMessage(code));
    }

    /// Evaluates @p chain for @p serverName with the SSL chain policy. Blocking.
    [[nodiscard]] std::expected<void, TlsFailure> evaluate(std::vector<DerCertificate> const& chain,
                                                           std::string const& serverName)
    {
        if (chain.empty())
            return refusal("the peer presented no certificate");
        auto const store =
            StorePtr { CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr) };
        if (!store)
            return refusal(describe("CertOpenStore", GetLastError()));

        // The leaf is the first certificate; the rest are intermediates the engine may use.
        auto leaf = CertificatePtr {};
        for (auto const& der: chain)
        {
            if (der.size() > std::numeric_limits<DWORD>::max())
                return refusal("a certificate in the presented chain is too large");
            auto added = PCCERT_CONTEXT { nullptr };
            if (CertAddEncodedCertificateToStore(store.get(),
                                                 X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                                 reinterpret_cast<BYTE const*>(der.data()),
                                                 static_cast<DWORD>(der.size()),
                                                 CERT_STORE_ADD_ALWAYS,
                                                 leaf ? nullptr : &added)
                == FALSE)
                return refusal(
                    describe("a certificate in the presented chain does not parse", GetLastError()));
            if (!leaf)
                leaf = CertificatePtr { added };
        }

        // Built for server authentication: a certificate whose EKU says otherwise does not chain.
        auto serverAuth = std::string { szOID_PKIX_KP_SERVER_AUTH };
        auto* usage = serverAuth.data();
        auto parameters = CERT_CHAIN_PARA {};
        parameters.cbSize = static_cast<DWORD>(sizeof(parameters));
        parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
        parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
        auto rawChain = PCCERT_CHAIN_CONTEXT { nullptr };
        if (CertGetCertificateChain(
                nullptr, leaf.get(), nullptr, store.get(), &parameters, chainBuildFlags(), nullptr, &rawChain)
            == FALSE)
            return refusal(describe("CertGetCertificateChain", GetLastError()));
        auto const built = ChainPtr { rawChain };

        auto wideName = std::wstring {};
        if (!serverName.empty())
        {
            auto converted = core::toUtf16(serverName);
            if (!converted)
                return refusal("the server name is not UTF-8");
            wideName = std::move(*converted);
        }
        auto ssl = SSL_EXTRA_CERT_CHAIN_POLICY_PARA {};
        ssl.cbSize = static_cast<DWORD>(sizeof(ssl));
        ssl.dwAuthType = AUTHTYPE_SERVER;
        // An empty name checks the chain alone.
        ssl.fdwChecks = serverName.empty() ? IgnoreServerName : 0;
        ssl.pwszServerName = serverName.empty() ? nullptr : wideName.data();
        auto policy = CERT_CHAIN_POLICY_PARA {};
        policy.cbSize = static_cast<DWORD>(sizeof(policy));
        policy.pvExtraPolicyPara = &ssl;
        auto status = CERT_CHAIN_POLICY_STATUS {};
        status.cbSize = static_cast<DWORD>(sizeof(status));
        if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, built.get(), &policy, &status) == FALSE)
            return refusal(describe("CertVerifyCertificateChainPolicy", GetLastError()));
        if (status.dwError != 0)
            return refusal(describe("the SSL chain policy refused the chain", status.dwError));
        return {};
    }

    class WindowsChainVerifier final: public IChainVerifier
    {
      public:
        explicit WindowsChainVerifier(async::IExecutor& blocking) noexcept: _blocking { &blocking } {}

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

std::uint32_t chainBuildFlags() noexcept
{
    return CERT_CHAIN_DISABLE_AIA;
}

std::shared_ptr<IChainVerifier> makeWindowsChainVerifier(async::IExecutor& blocking)
{
    return std::make_shared<WindowsChainVerifier>(blocking);
}

} // namespace core::net::detail
