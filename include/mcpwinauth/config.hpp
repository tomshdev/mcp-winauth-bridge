// Configuration, sinks and error types. No Windows headers: these types cross
// into host translation units.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mcpwinauth {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Emits one complete JSON-RPC message. Called from worker threads, so
// implementations must be thread-safe.
using MessageSink = std::function<void(const std::string& message)>;

// Called from worker threads; must be thread-safe.
using LogSink = std::function<void(LogLevel level, const std::string& text)>;

// TLS checks that can be waived. Split rather than a single on/off switch so
// an IP-address URL can waive only the name check and still require a
// trusted chain, which is a good deal safer than waiving everything.
enum TlsIgnore : unsigned {
    TlsIgnoreNothing      = 0,
    TlsIgnoreUnknownCa    = 1u << 0,  // self-signed, or a CA we do not trust
    TlsIgnoreNameMismatch = 1u << 1,  // CN/SAN does not match, e.g. an IP URL
    TlsIgnoreExpired      = 1u << 2,
    TlsIgnoreWrongUsage   = 1u << 3,
    TlsIgnoreEverything   = 0xFu,
};

// Waiving either of these means the peer is no longer identified, which is
// what the credential policy keys off: the rest still leave the chain and
// the name proven.
const unsigned kTlsIgnoreDefeatsIdentity = TlsIgnoreUnknownCa | TlsIgnoreNameMismatch;

struct Timeouts {
    int resolveMs = 10000;
    int connectMs = 10000;
    int sendMs    = 30000;
    int receiveMs = 600000;  // long, so a slow tool call is not cut off
};

struct Config {
    std::wstring url;  // required
    std::wstring userAgent = L"mcp-winauth-bridge/1.0";
    Timeouts     timeouts{};

    unsigned workerCount    = 4;    // size of the bounded pool, >= 1
    unsigned maxQueueDepth  = 256;  // 0 = unbounded, otherwise Submit() blocks
    unsigned maxAuthRetries = 3;    // attempts in the 401 loop

    // Send the logged-in user's credentials to hosts outside the Intranet
    // zone. Needed for most corporate endpoints, which are rarely zoned.
    bool autologonAnyHost = true;
    // ...and to a host whose identity was not established: plain http, or
    // https with the CA or name check waived. In either case a Negotiate or
    // NTLM exchange can be intercepted and relayed. Off unless you ask.
    bool allowInsecureAuth = false;

    // Bitmask of TlsIgnore. Zero means every certificate check applies.
    unsigned tlsIgnore = TlsIgnoreNothing;

    bool     deleteSessionOnShutdown = true;
    unsigned drainTimeoutMs          = 0;  // 0 = wait for in-flight forever

    LogLevel logLevel = LogLevel::Info;
};

enum class Status {
    Ok = 0,
    InvalidArgument,
    BadUrl,
    TransportInitFailed,
    AlreadyStarted,
    NotStarted,
};

struct Error {
    Status        status = Status::Ok;
    unsigned long win32  = 0;  // GetLastError() where relevant, else 0
    std::string   message;

    explicit operator bool() const { return status == Status::Ok; }
};

Error MakeError(Status status, std::string message, unsigned long win32 = 0);

// Ready-made sinks matching the standalone exe's behaviour.
MessageSink MakeStdoutLineSink();                  // mutex-guarded, one line, flushed
LogSink     MakeStderrLogSink(LogLevel minLevel);  // "[bridge] ..." on stderr

const char* ToString(LogLevel level);

// Human-readable list of the waived TLS checks, for the startup warning.
std::string DescribeTlsIgnore(unsigned tlsIgnoreMask);

}  // namespace mcpwinauth
