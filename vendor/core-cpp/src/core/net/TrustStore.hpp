// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a verifying TLS client's trust comes from: a PEM bundle in memory, the bundle or hashed
/// directory a POSIX system keeps, or -- on macOS and Windows -- the platform's own verifier.
///
/// **Every source fails closed.** An environment variable that names a file that is not there, an
/// empty store, a file that cannot be read and a bundle that does not parse are each an error, never
/// a quiet fallback to the next source and never a context that trusts nothing in particular.
///
/// Reading files is blocking, so a store that reads them does it on an injected blocking executor
/// through @c async::offload, and answers on the home one: a caller on a loop never waits on a disk.

#include <core/Environment.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/Task.hpp>
#include <core/net/NetTlsExport.hpp>
#include <core/net/Tls.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace core::net
{

/// Loads the anchors a verifying client trusts.
class ITrustStore
{
  public:
    ITrustStore() = default;
    virtual ~ITrustStore() = default;
    ITrustStore(ITrustStore const&) = delete;
    ITrustStore& operator=(ITrustStore const&) = delete;
    ITrustStore(ITrustStore&&) = delete;
    ITrustStore& operator=(ITrustStore&&) = delete;

    /// Loads the anchors, off the loop where it reads files. The store must outlive the task.
    /// @return The anchors, at least one; or a human-readable reason naming the source that failed.
    [[nodiscard]] virtual async::Task<std::expected<TrustAnchors, std::string>> load() = 0;
};

/// A store over a PEM bundle held in memory; it reads no file, so it answers without a hop.
/// @param pem One or more PEM certificates.
/// @return The store. Its @c load answers what @c trustAnchorsFromPem answers for @p pem.
[[nodiscard]] CORE_NET_TLS_API std::unique_ptr<ITrustStore> makePemTrustStore(std::string pem);

/// What a trust location on disk holds.
enum class TrustBundleKind : std::uint8_t
{
    /// One file of concatenated PEM certificates.
    Bundle = 0,
    /// A directory OpenSSL's `c_rehash`/`certctl` keeps: each certificate as `<subject hash>.<n>`
    /// (eight lower-case hex digits, a dot, a decimal number). Only files named so are read; the
    /// PEM files they link to, CRLs (`<hash>.r<n>`) and anything else are not.
    HashedDirectory,
};

/// One place a system keeps its trust anchors.
struct TrustBundleLocation
{
    std::string_view path;         ///< Where, absolute.
    std::string_view distribution; ///< Which systems keep it there, for the reader of a failure.
    TrustBundleKind kind = TrustBundleKind::Bundle; ///< A bundle file or a hashed directory.
};

/// The trust locations @c makeFileTrustStore looks in, in the order it looks. Every bundle comes
/// before any directory: where a system has both (Debian's `/etc/ssl/certs` holds its bundle), the
/// bundle is the one file to read.
/// @return The table; static, and the same on every OS.
[[nodiscard]] CORE_NET_TLS_API std::span<TrustBundleLocation const> trustBundleLocations() noexcept;

/// The largest bundle file @c makeFileTrustStore reads: 16 MiB. A system bundle of every public
/// root is about 0.25 MiB, so this is room for a large private one, not a limit anyone meets; what
/// it stops is a misnamed file (a log, a disk image) being read whole into memory.
inline constexpr auto MaxTrustBundleBytes = std::uintmax_t { 16 } * 1024 * 1024;

/// The most a hashed directory's files may hold together: 16 MiB, as @c MaxTrustBundleBytes.
inline constexpr auto MaxTrustDirectoryBytes = std::uintmax_t { 16 } * 1024 * 1024;

/// A store over the files a POSIX system keeps its anchors in, as OpenSSL finds them.
///
/// The first of these that is SET decides, and a later one is never consulted:
/// 1. `SSL_CERT_FILE`, a bundle. It must be a regular file that can be read.
/// 2. `SSL_CERT_DIR`, ONE hashed directory (a `:`-separated list is not split, so it names no
///    directory and is refused). It must hold at least one hashed file, and every one must be read.
/// 3. Otherwise, the first row of @c trustBundleLocations that exists -- a regular file for a
///    bundle, a directory for a hashed directory -- read as above; a row that exists but does not
///    read is the answer, not a reason to try the next.
///
/// A bundle larger than @c MaxTrustBundleBytes, or a directory whose hashed files together exceed
/// @c MaxTrustDirectoryBytes, is refused before it is read; the sizes come from
/// @c platform::FileSystem::fileSize.
///
/// **What refusing a whole directory costs.** OpenSSL skips a hashed file it cannot use; this
/// refuses the directory, so ONE broken link or corrupt file in `/etc/ssl/certs` -- a package
/// half-removed, a typo in a hand-added certificate -- takes away every anchor until it is fixed,
/// and every verifying connection fails rather than some. That is the price of the alternative's
/// failure being silent: a skipped file is an anchor the operator believes is trusted and is not,
/// or the one certificate that made a private service reachable. The error names the file.
///
/// A variable set to the empty string counts as unset. Portable: it reads through @p files, so it
/// runs, and is tested, on every OS.
/// @param files Where the files are read; must outlive the store.
/// @param environment Where the two variables are read; must outlive the store.
/// @param home Where @c ITrustStore::load resumes; must outlive the store.
/// @param blocking Where the files are read; must outlive the store.
/// @return The store.
[[nodiscard]] CORE_NET_TLS_API std::unique_ptr<ITrustStore> makeFileTrustStore(
    platform::FileSystem const& files,
    Environment const& environment,
    async::IExecutor& home,
    async::IExecutor& blocking);

/// Builds the verifying client context this operating system's own trust settings call for.
///
/// - **macOS** (Security.framework) and **Windows** (CryptoAPI): a platform @c IChainVerifier, which
///   runs and finishes on @p blocking -- each socket's wait for it is handed back to that socket's
///   own loop -- so the Disallowed store and the user's trust settings are honoured. @p files and
///   @p environment are not read.
/// - **Every other system**: the anchors @c makeFileTrustStore loads over @p files and
///   @p environment, read on @p blocking.
///
/// **An incomplete chain is refused everywhere.** No platform fetches a missing intermediate from
/// the Authority Information Access URL a server's certificate names: OpenSSL never does, and the
/// platform verifiers turn it off (`CERT_CHAIN_DISABLE_AIA` on Windows,
/// `SecTrustSetNetworkFetchAllowed(false)` on macOS), so verifying a chain sends no request to a host
/// the server chose. A server must send its intermediates.
///
/// One CMake-selected `SystemTrust.cpp` decides, never an `#ifdef`.
/// @param files Where bundles are read; non-null where it is read, and outliving the task.
/// @param environment Where `SSL_CERT_FILE`/`SSL_CERT_DIR` are read; as @p files.
/// @param home Where THIS task resumes once the anchors are read, and nothing more: the factory's
///        own resume point, outliving the task. The context does not keep it, and its sockets'
///        verifications never resume on it; each socket's waiters go back to the loop it was
///        wrapped on. Non-null.
/// @param blocking Where files are read and chains verified. Non-null, and outliving the context.
/// @param options The protocol floor and the ALPN names offered.
/// @return The context, or why there is nothing to trust (no configuration found, a file that does
///         not read).
[[nodiscard]] CORE_NET_TLS_API async::Task<std::expected<std::shared_ptr<ITlsContext>, std::string>>
makeSystemTlsClientContext(platform::FileSystem const* files,
                           Environment const* environment,
                           async::IExecutor* home,
                           async::IExecutor* blocking,
                           TlsClientOptions options);

} // namespace core::net
