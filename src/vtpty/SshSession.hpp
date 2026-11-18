// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/Pty.hpp>
#include <vtpty/SshFailureMessage.hpp>
#include <vtpty/SshSessionRules.hpp>
#include <vtpty/SshTerminfoInstall.hpp>

#include <crispy/FileDescriptor.hpp>

#include <core/Overloaded.hpp>
#include <core/log/LogStore.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace vtpty
{

/// How long an SSH connection may take to come up when the profile does not say.
inline constexpr auto DefaultSshConnectTimeout = std::chrono::seconds { 30 };

struct SshHostConfig
{
    using Environment = std::map<std::string, std::string>;

    std::string hostname;
    int port = 22;
    std::string username;
    std::filesystem::path privateKeyFile;
    std::filesystem::path publicKeyFile;
    std::filesystem::path knownHostsFile;
    bool forwardAgent = false;
    /// Whether the built-in SSH profile installs Contour's terminfo on the remote host.
    /// This is the configured value; the pinned-TERM rule is applied at session creation.
    TerminfoInstall terminfoInstall = TerminfoInstall::Install;
    /// The longest the connection may take to come up, from the TCP connect to the started shell, not
    /// counting the time spent waiting for the user (host key confirmation, passphrase, password).
    /// nullopt means no limit.
    std::optional<std::chrono::seconds> connectTimeout = DefaultSshConnectTimeout;
    Environment env;

    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toConfigString(std::string const& host = {}) const;
    [[nodiscard]] std::string toKnownhostComment() const;
};

using SshHostConfigMap = std::map<std::string, SshHostConfig>;

/// A host's key pair as read from its files, which is how libssh2 takes it in memory.
struct SshKeyFiles
{
    std::string publicKey;  ///< The public key file's text; empty where the host names none.
    std::string privateKey; ///< The private key file's text.
};

/// Reads @p config's key files through their paths. libssh2 would open each by a narrow name, which
/// Windows reads in the ANSI code page, so that a key file beyond it is handed over in memory instead.
/// @param config The host, naming its key files.
/// @return Both files' text; the error of the first that cannot be read.
[[nodiscard]] std::expected<SshKeyFiles, std::error_code> readSshKeyFiles(SshHostConfig const& config);

std::expected<SshHostConfigMap, std::error_code> loadSshConfig(std::filesystem::path const& configPath);
std::expected<SshHostConfigMap, std::error_code> loadSshConfig();

/// SSH Host Key information.
///
/// This structure holds the SSH host key type and its raw binary data.
struct SshHostkey
{
    /// key type, e.g., LIBSSH2_HOSTKEY_TYPE_RSA
    int type;

    /// raw binary key data
    std::vector<char> data;
};

struct SshHostkeyHash
{
    int keyType;

    /// raw binary fingerprint data
    std::vector<char> fingerprint;

    explicit SshHostkeyHash(int keyType, char const* data, size_t length):
        keyType { keyType }, fingerprint { data ? data : "", data ? data + length : nullptr }
    {
    }

    [[nodiscard]] std::string toString() const
    {
        std::string result;
        for (size_t i = 0; i < fingerprint.size(); ++i)
        {
            result += std::format("{:02X}", static_cast<unsigned char>(fingerprint[i]));
            if (i + 1 < fingerprint.size())
                result += ":";
        }
        return result;
    }
};

/// SSH Host Key Verification Request.
///
/// This structure is used to request user verification of the SSH host key
/// during the SSH handshake process.
struct SshHostkeyVerificationRequest
{
    /// Hostname (and possibly port) of the SSH server to connect to.
    std::string hostname;

    /// Port of the SSH server to connect to.
    int port;

    /// Host key hash information.
    SshHostkeyHash hostkeyHash;
};

/// Callback type for responding to host key verification requests.
using SshHostkeyVerificationResponseCallback = std::function<void(bool accepted)>;

/// Callback type for host key verification requests.
using SshHostkeyVerificationRequestCallback = std::function<void(
    SshHostkeyVerificationRequest const& request, SshHostkeyVerificationResponseCallback const& response)>;

/// SSH Login session.
class SshSession final: public Pty
{
  public:
    // clang-format off
    struct NormalExit { int exitCode; };
    struct SignalExit { std::string signal; std::string errorMessage; std::string languageTag; };
    // clang-format on

    using ExitStatus = std::variant<NormalExit, SignalExit>;
    using Environment = std::map<std::string, std::string>;

    /// @param config                  The host to connect to.
    /// @param hostkeyRequestCallback  Asks the user to trust an unknown host key.
    /// @param terminfo                When engaged, Contour's terminfo is installed on the host before the
    ///                                shell starts, unless the host's earlier install failed.
    SshSession(SshHostConfig config,
               SshHostkeyVerificationRequestCallback hostkeyRequestCallback,
               std::optional<SshTerminfoInstaller> terminfo = std::nullopt);
    ~SshSession() override;

    [[nodiscard]] StartResult start() override;
    PtySlave& slave() noexcept override;
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    void wakeupReader() override;
    [[nodiscard]] int write(std::string_view buf) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;

    [[nodiscard]] bool isOperational() const noexcept;

    [[nodiscard]] std::optional<ExitStatus> exitStatus() const;

    enum class State : uint8_t
    {
        Initial,                            // initial state
        Started,                            // start() has been called
        Connect,                            // connect to SSH server (usually TCP/IPv4 or TCP/IPv6)
        Handshake,                          // SSH handshake
        VerifyHostkey,                      // verify host key against known_hosts file
        VerifyHostkeyWaitForInput,          // wait for user confirmation of host key
        AuthenticateAgent,                  // authenticate with SSH agent
        AuthenticatePrivateKeyStart,        // start private key authentication
        AuthenticatePrivateKeyRequest,      // request private key's password
        AuthenticatePrivateKeyWaitForInput, // wait for private key's password (user input))
        AuthenticatePrivateKey,             // authenticate with private key
        AuthenticatePasswordStart,          // start password authentication
        AuthenticatePasswordWaitForInput,   // wait for password (user input)
        AuthenticatePassword,               // authenticate with password
        InstallTerminfoOpenChannel,         // open the channel that installs Contour's terminfo
        InstallTerminfoExec,                // run the install command
        InstallTerminfoRead,                // read its output and judge it
        OpenChannel,                        // open SSH channel
        RequestAuthAgent,                   // request SSH (auth) agent forwarding
        RequestPty,                         // request pty
        SetEnv,                             // set environment variables
        StartShell,                         // shell is starting
        Operational,                        // setup done, and shell is running
        ResizeScreen,                       // resize screen, then move to operational
        Failure,                            // connection closed with protocol related error
        Closed,                             // connection closed by peer or us
    };

    // Wait for the socket to become readable and/or writable.
    int waitForSocket(std::optional<std::chrono::milliseconds> timeout = std::nullopt);

  private:
    bool connect(std::string_view host, int port);
    void logErrorWithDetails(int libssl2ErrorCode, std::string_view message) const;

    enum class HostkeyVerificationStatus : uint8_t
    {
        Skipped,
        FailedWithMismatch,
        FailedToReadKnownHosts,
        FailedByUserTrust,
        Verified,
        WaitingForUserConfirmation,
    };

    /// Verifies the remote host key against the known_hosts file.
    ///
    /// If the host key is not found, the user is prompted to accept it.
    /// If the host key does not match, the connection is aborted.
    ///
    /// @return true if the host key is verified, false otherwise.
    [[nodiscard]] HostkeyVerificationStatus verifyHostkey();

    void hostkeyVerificationResultCallback(bool accepted);

    bool authenticateWithAgent();

    /// Moves to whichever authentication method follows the agent.
    ///
    /// Reached both when the agent has no more identities to offer and when there was never an agent
    /// to ask -- the case inside a Flatpak, where $SSH_AUTH_SOCK names a host path the sandbox does
    /// not mount. Those failures used to return with the state still on AuthenticateAgent, so the
    /// next read re-entered the same case and libssh2_agent_get_identity() was called on an agent
    /// that had never connected.
    void fallBackFromAgent();

    /// Judges a failed agent request and acts on it: an agent whose allowance ran out ends the session, one
    /// that went away is reported and left for the next way to authenticate. @see classifySshAgentFailure()
    /// @param rc The failed libssh2 call's result.
    /// @return How it failed; the caller goes on as before only for SshAgentFailure::Failed.
    [[nodiscard]] SshAgentFailure judgeAgentFailure(int rc);
    void authenticateWithPrivateKey();
    void authenticateWithPassword();

    void handlePreAuthenticationPasswordInput(std::string_view buf, State next);

    void injectRead(std::string_view buf);
    void injectWrite(std::string_view buf);

    // Handles each individual states.
    void processState();

    /// Whether a step of the connect sequence lets processState() go on to the next state.
    enum class ConnectStep : uint8_t
    {
        Stop = 0, ///< Stop here: the step failed, ended the session, or must be retried later.
        Next,     ///< The step is done and the state moved on.
    };

    /// Runs the SSH handshake. @return Whether to go on.
    [[nodiscard]] ConnectStep handshake();

    /// Opens the shell's session channel. @return Whether to go on.
    [[nodiscard]] ConnectStep openChannel();

    /// Asks for agent forwarding where configured; a refusal is logged, not fatal. @return Whether to go on.
    [[nodiscard]] ConnectStep requestAuthAgent();

    /// Starts the shell and makes the session operational: no timeout, non-blocking.
    void startShell();

    // some of the complex states to handle
    bool requestPty();

    // The terminfo install, run between authentication and the shell's channel. Each step runs to
    // completion (the session is blocking here) and ends in leaveTerminfoInstall(), which either moves on
    // to the shell's channel or, when a timeout left the connection unable to open one, ends the session.
    // An install that merely does not work never fails the connection. @see SshTerminfoInstaller.
    void openTerminfoInstallChannel();
    void execTerminfoInstall();
    void readTerminfoInstall();

    /// The connection's deadline, as the connect budget stands now. @see SshConnectBudget
    /// @return It, or nullopt when no limit applies.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> connectDeadline() const;

    /// The deadline of the current step: the connect budget's, or in State::AuthenticateAgent the agent
    /// allowance's. @see sshStepBudgetFor()
    /// @return It, or nullopt when no limit applies.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> stepDeadline() const;

    /// Arms libssh2's timeout for the next blocking call of the connect sequence with what is left of the
    /// current step's budget, or, once it is spent, ends the session with failStepTimeout().
    /// @return Whether anything was left; the caller stops when not.
    [[nodiscard]] SshBudget armStepTimeout();

    /// Judges a failed call of the connect sequence and, if it timed out, ends the session with
    /// failStepTimeout(). @see classifySshStepFailure()
    /// @param rc The failed libssh2 call's result.
    /// @return TimedOut if the session was ended; Failed if the caller handles the failure as before.
    [[nodiscard]] SshStepFailure failIfStepTimedOut(int rc);

    /// Ends the session because the current step ran out of its budget: ssh.connect_timeout, naming the stage
    /// it had reached, or the agent's allowance. The session is switched to non-blocking first: a call that
    /// timed out can leave libssh2 half-way through a request, and nothing done with the session afterwards
    /// -- close(), the destructor's teardown -- may wait on a host that already took too long.
    /// @see sshTeardownFor()
    void failStepTimeout();

    /// Arms libssh2's timeout with what is left of the install budget.
    /// @return Whether anything was left to arm it with.
    [[nodiscard]] SshBudget armTerminfoTimeout();

    /// Ends the install step, reached at @p stage, with @p outcome: cleans up the channel, reports, remembers
    /// a failed host and then, per afterTerminfoInstall(), moves on to State::OpenChannel or ends the
    /// session. @see the definition for what a timeout leaves behind.
    void leaveTerminfoInstall(TerminfoInstallStage stage, TerminfoInstallOutcome outcome);

    /// Judges a failed install step. @see classifySshStepFailure()
    /// @param rc The failed libssh2 call's result.
    /// @return TimedOut if libssh2's timeout fired or the install deadline has passed, else Failed.
    [[nodiscard]] SshStepFailure classifyTerminfoFailure(int rc) const;

    /// @return The TERM used when the terminfo entry is not known to be installed.
    [[nodiscard]] std::string fallbackTerm() const;
    bool setEnv();
    void resizeScreen();

    void setState(State nextState);

    void logInfo(std::string_view message) const;
    void logInject(std::string_view message) const;
    void logInfoWithInject(std::string_view message) const;

    template <typename... Args>
    void logInfoWithInject(std::format_string<Args...> fmt, Args&&... args) const
    {
        logInfoWithInject(std::format(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void logInfo(std::format_string<Args...> fmt, Args&&... args) const
    {
        logInfo(std::format(fmt, std::forward<Args>(args)...));
    }

    void logError(std::string_view message) const;

    template <typename... Args>
    void logError(std::format_string<Args...> fmt, Args&&... args) const
    {
        logError(std::format(fmt, std::forward<Args>(args)...));
    }

    /// Reports @p message to the error log AND onto the screen.
    ///
    /// logError() alone reaches only the error log, which is off by default -- so every way an SSH
    /// session could fail to come up used to be invisible, and the user saw the session layer's
    /// generic "Shell terminated too quickly." with no host, port or reason in it. The three
    /// `// TODO candidate of a future logErrorWithInject()` comments in the source named this.
    void logErrorWithInject(std::string_view message) const;

    /// Ends this session with @p reason: reports it, records it, and moves to State::Failure.
    ///
    /// Recording matters because start() reads the reason back: processState() runs synchronously
    /// far enough to attempt the connection, so a host that is not there is known BEFORE start()
    /// returns, and can be reported as the StartFailure the session layer already knows how to show.
    ///
    /// The socket is shut down directly rather than through close(), which takes _mutex -- and every
    /// caller of this already holds it, processState() running under the lock in both start() and
    /// read(). It is only shut down, not closed: libssh2 still sends its DISCONNECT/CLOSE through it when
    /// the session is freed, and a closed descriptor number may by then belong to another socket. The
    /// destructor closes it, after the session is freed. @see _mutex.
    void fail(std::string reason);

    /// Shuts the connection down in both directions, keeping the descriptor open until the destructor.
    /// The shutdown is what tells the peer, and any poll() on the socket, that we are gone.
    void shutdownSocket();

    template <typename... Args>
    void fail(std::format_string<Args...> fmt, Args&&... args)
    {
        fail(std::format(fmt, std::forward<Args>(args)...));
    }

    SshHostConfig _config;

    /// Installs Contour's terminfo before the shell, when engaged. Fixed at construction.
    std::optional<SshTerminfoInstaller> _terminfo;

    /// How the install ended; decides the TERM that requestPty() sends.
    TerminfoInstallOutcome _terminfoOutcome = TerminfoInstallOutcome::NotAttempted;

    /// Whether an install channel was opened, for the channel-open failure message.
    TerminfoInstallAttempt _terminfoAttempt = TerminfoInstallAttempt::None;

    /// The tail of the install command's standard output. Only the marker line matters.
    std::string _terminfoOutput;

    /// The end of the install budget. Set when the install channel is opened.
    std::chrono::steady_clock::time_point _terminfoDeadline {};

    /// The limit on the connection coming up. Engaged by start(), when its clock starts; setState() stops
    /// that clock while the session waits for the user.
    std::optional<SshConnectBudget> _connectBudget;

    /// The end of the agent authentication step's own allowance; set by setState() on entering
    /// State::AuthenticateAgent, nullopt without a connect limit. @see SshAgentAuthAllowance
    std::optional<std::chrono::steady_clock::time_point> _agentDeadline;

    /// Why this session failed, for start() to hand back. Empty until something fails.
    std::string _failureReason;

    std::optional<bool> _hostkeyVerified = std::nullopt;
    SshHostkeyVerificationRequestCallback _hostkeyRequestCallback;

    PageSize _pageSize { .lines = LineCount(24), .columns = ColumnCount(80) };
    std::optional<ImageSize> _pixels = std::nullopt;
    std::unique_ptr<PtySlave> _ptySlave;

    /// Serializes every libssh2 call, and with them the state machine that issues them.
    ///
    /// A single LIBSSH2_SESSION must not be used from two threads at once: libssh2 keeps the
    /// transport's cipher and MAC state per session, so an overlapping call corrupts it and the
    /// connection dies with LIBSSH2_ERROR_DECRYPT / LIBSSH2_ERROR_OUT_OF_BOUNDARY. That is exactly
    /// what happens here without it -- the PTY reader thread sits in read() while the GUI thread
    /// enters write() on a keystroke or resizeScreen() on a window resize.
    ///
    /// Lock ordering: _mutex may be taken before _injectMutex (processState() does, on the way to
    /// State::Operational), never after. read() therefore releases _injectMutex before it touches
    /// libssh2 at all -- which it must anyway, since holding it across a blocking read would stall
    /// every GUI-thread injectRead() until the remote sends a byte.
    ///
    /// Mutable so the const exitStatus() can hold it.
    std::mutex mutable _mutex;

    struct Private;
    std::unique_ptr<Private, void (*)(Private*)> _p;

    /// Atomic because it is read without _mutex: isClosed() is called from the PTY reader thread and
    /// from TerminalSession teardown, and read()'s condition-variable predicate examines it under
    /// _injectMutex instead. setState() writes it under _mutex, so those reads would otherwise race.
    ///
    /// _walkIndex needs no such treatment: every access sits inside processState() or the auth helpers
    /// it calls, all of which run under _mutex.
    std::atomic<State> _state = State::Initial;
    int _walkIndex = 0;

    std::string _injectedWrite;
    std::string _injectedRead;
    std::mutex _injectMutex;
    std::condition_variable _injectCV;

    std::mutex _closedMutex;
    std::condition_variable _closedCV;
};

/// Which time limit governs @p state: the connect budget, the agent's own allowance, or none while the
/// session waits for the user or is not coming up.
/// @param state A state of the session.
/// @return The budget that applies.
[[nodiscard]] constexpr SshStepBudget sshStepBudgetFor(SshSession::State state) noexcept
{
    using State = SshSession::State;
    switch (state)
    {
        case State::AuthenticateAgent: return SshStepBudget::Agent;
        case State::VerifyHostkeyWaitForInput:
        case State::AuthenticatePrivateKeyWaitForInput:
        case State::AuthenticatePasswordWaitForInput:
        case State::Initial:
        case State::Operational:
        case State::ResizeScreen:
        case State::Failure:
        case State::Closed: return SshStepBudget::None;
        case State::Started:
        case State::Connect:
        case State::Handshake:
        case State::VerifyHostkey:
        case State::AuthenticatePrivateKeyStart:
        case State::AuthenticatePrivateKeyRequest:
        case State::AuthenticatePrivateKey:
        case State::AuthenticatePasswordStart:
        case State::AuthenticatePassword:
        case State::InstallTerminfoOpenChannel:
        case State::InstallTerminfoExec:
        case State::InstallTerminfoRead:
        case State::OpenChannel:
        case State::RequestAuthAgent:
        case State::RequestPty:
        case State::SetEnv:
        case State::StartShell: return SshStepBudget::Connect;
    }
    return SshStepBudget::None;
}

/// What the connection is doing in @p state, for the connect-timeout message.
/// @param state A state of the session.
/// @return A phrase such as "the SSH handshake"; "connecting" for a state outside the connect sequence.
[[nodiscard]] constexpr std::string_view connectStageName(SshSession::State state) noexcept
{
    using State = SshSession::State;
    switch (state)
    {
        case State::Started:
        case State::Connect: return "the TCP connect";
        case State::Handshake: return "the SSH handshake";
        case State::VerifyHostkey:
        case State::VerifyHostkeyWaitForInput: return "host key verification";
        case State::AuthenticateAgent:
        case State::AuthenticatePrivateKeyStart:
        case State::AuthenticatePrivateKeyRequest:
        case State::AuthenticatePrivateKeyWaitForInput:
        case State::AuthenticatePrivateKey:
        case State::AuthenticatePasswordStart:
        case State::AuthenticatePasswordWaitForInput:
        case State::AuthenticatePassword: return "authentication";
        case State::InstallTerminfoOpenChannel:
        case State::InstallTerminfoExec:
        case State::InstallTerminfoRead: return "the terminfo install";
        case State::OpenChannel: return "opening the session channel";
        case State::RequestAuthAgent: return "the agent forwarding request";
        case State::RequestPty: return "the PTY request";
        case State::SetEnv: return "sending the environment";
        case State::StartShell: return "starting the shell";
        case State::Initial:
        case State::Operational:
        case State::ResizeScreen:
        case State::Failure:
        case State::Closed: break;
    }
    return "connecting";
}

} // namespace vtpty

template <>
struct std::formatter<vtpty::SshSession::State>: std::formatter<std::string_view>
{
    auto format(vtpty::SshSession::State const& state, auto& ctx) const
    {
        std::string_view name;
        // clang-format off
        switch (state)
        {
            case vtpty::SshSession::State::Initial: name = "Initial"; break;
            case vtpty::SshSession::State::Started: name = "Started"; break;
            case vtpty::SshSession::State::Connect: name = "Connect"; break;
            case vtpty::SshSession::State::Handshake: name = "Handshake"; break;
            case vtpty::SshSession::State::VerifyHostkey: name = "VerifyHostkey"; break;
            case vtpty::SshSession::State::VerifyHostkeyWaitForInput: name = "VerifyHostkeyWaitForInput"; break;
            case vtpty::SshSession::State::AuthenticateAgent: name = "AuthenticateAgent"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKeyStart: name = "AuthenticatePrivateKeyStart"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKeyRequest: name = "AuthenticatePrivateKeyRequest"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKeyWaitForInput: name = "AuthenticatePrivateKeyWaitForInput"; break;
            case vtpty::SshSession::State::AuthenticatePrivateKey: name = "AuthenticatePrivateKey"; break;
            case vtpty::SshSession::State::AuthenticatePasswordStart: name = "AuthenticatePasswordStart"; break;
            case vtpty::SshSession::State::AuthenticatePasswordWaitForInput: name = "AuthenticatePasswordWaitForInput"; break;
            case vtpty::SshSession::State::AuthenticatePassword: name = "AuthenticatePassword"; break;
            case vtpty::SshSession::State::InstallTerminfoOpenChannel: name = "InstallTerminfoOpenChannel"; break;
            case vtpty::SshSession::State::InstallTerminfoExec: name = "InstallTerminfoExec"; break;
            case vtpty::SshSession::State::InstallTerminfoRead: name = "InstallTerminfoRead"; break;
            case vtpty::SshSession::State::OpenChannel: name = "OpenChannel"; break;
            case vtpty::SshSession::State::RequestAuthAgent: name = "RequestAuthAgent"; break;
            case vtpty::SshSession::State::RequestPty: name = "RequestPty"; break;
            case vtpty::SshSession::State::SetEnv: name = "SetEnv"; break;
            case vtpty::SshSession::State::StartShell: name = "StartShell"; break;
            case vtpty::SshSession::State::Operational: name = "Operational"; break;
            case vtpty::SshSession::State::ResizeScreen: name = "ResizeScreen"; break;
            case vtpty::SshSession::State::Failure: name = "Failure"; break;
            case vtpty::SshSession::State::Closed: name = "Closed"; break;
        }
        // clang-format on
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtpty::SshSession::ExitStatus>: std::formatter<std::string>
{
    auto format(vtpty::SshSession::ExitStatus const& status, auto& ctx) const
    {
        return std::visit(core::Overloaded { [&](vtpty::SshSession::NormalExit exit) {
                                                return std::formatter<std::string>::format(
                                                    std::format("{} (normal exit)", exit.exitCode), ctx);
                                            },
                                             [&](vtpty::SshSession::SignalExit exit) {
                                                 return std::formatter<std::string>::format(
                                                     std::format("{} ({})", exit.signal, exit.errorMessage),
                                                     ctx);
                                             } },
                          status);
    }
};

template <>
struct std::formatter<vtpty::SshHostConfig>: std::formatter<std::string>
{
    auto format(vtpty::SshHostConfig const& config, auto& ctx) const
    {
        return std::formatter<std::string>::format(config.toString(), ctx);
    }
};
