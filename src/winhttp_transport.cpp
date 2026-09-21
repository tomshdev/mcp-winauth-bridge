// The only file in the project that talks to the network. It contains no
// parsing: the response body is handed to the caller as raw chunks.
#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>

#include "mcpwinauth/headers.hpp"
#include "mcpwinauth/transport.hpp"

#pragma comment(lib, "winhttp.lib")

namespace mcpwinauth {
namespace {

// Control characters are mapped out too, not just non-ASCII: a CR or LF that
// survived into a header value would let a server inject headers downstream.
std::string NarrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t c : s) {
        const bool printable = c >= 0x20 && c < 0x7F;
        out += printable ? static_cast<char>(c) : '?';
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::wstring QueryHeader(HINTERNET req, const wchar_t* name) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return L"";

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, &value[0], &size,
                             WINHTTP_NO_HEADER_INDEX))
        return L"";
    value.resize(size / sizeof(wchar_t));
    // WinHTTP counts the terminating NUL in `size` on the way out.
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

// Closes an HINTERNET on scope exit.
struct HandleGuard {
    HINTERNET h = nullptr;
    ~HandleGuard() { if (h) WinHttpCloseHandle(h); }
};

class WinHttpTransport final : public ITransport {
public:
    WinHttpTransport(HINTERNET session, Config cfg, ParsedUrl target, LogSink log)
        : session_(session), cfg_(std::move(cfg)), target_(std::move(target)),
          log_(std::move(log)) {}

    ~WinHttpTransport() override { CloseSession(); }

    ResponseHead Request(HttpVerb           verb,
                         const std::string& body,
                         const std::string& sessionId,
                         const HeadFn&      onHead,
                         const BodyChunkFn& onChunk) override;

    void Abort() override {
        // Closing the session handle is the only safe way to release a thread
        // parked inside WinHttpReceiveResponse.
        CloseSession();
    }

private:
    void CloseSession() {
        HINTERNET s = nullptr;
        {
            std::lock_guard<std::mutex> lock(mx_);
            s        = session_;
            session_ = nullptr;
        }
        if (s) WinHttpCloseHandle(s);
    }

    void Log(LogLevel level, const std::string& text) const {
        if (log_) log_(level, text);
    }

    // Creates the connection handle while holding the lock. WinHttpConnect
    // does no network I/O, so this is brief, and it means Abort() can never
    // close the session handle out from under a caller that has snapshotted
    // it. Past this point we only touch our own child handles.
    HINTERNET Connect(unsigned long& win32) {
        std::lock_guard<std::mutex> lock(mx_);
        if (!session_) {
            // Not a connect failure: the session was closed under us, so
            // GetLastError() would be stale and misleading.
            win32 = ERROR_WINHTTP_OPERATION_CANCELLED;
            return nullptr;
        }
        HINTERNET conn = WinHttpConnect(session_, target_.host.c_str(), target_.port, 0);
        if (!conn) win32 = GetLastError();
        return conn;
    }

    // True when TLS actually proved who the server is. Waiving the CA or the
    // name check means it did not, and https then buys no more identity than
    // plain http does.
    bool PeerIdentityVerified() const {
        if (!target_.secure) return false;
        return (cfg_.tlsIgnore & kTlsIgnoreDefeatsIdentity) == 0;
    }

    mutable std::mutex mx_;
    HINTERNET          session_ = nullptr;
    Config             cfg_;
    ParsedUrl          target_;
    LogSink            log_;
    std::once_flag     insecureAuthWarned_;
    std::once_flag     tlsWaivedWarned_;
};

ResponseHead WinHttpTransport::Request(HttpVerb           verb,
                                       const std::string& body,
                                       const std::string& sessionId,
                                       const HeadFn&      onHead,
                                       const BodyChunkFn& onChunk) {
    ResponseHead head;

    HandleGuard conn;
    conn.h = Connect(head.win32Error);
    if (!conn.h) {
        Log(LogLevel::Error, "connect failed: " + std::to_string(head.win32Error));
        return head;
    }

    const wchar_t* method = (verb == HttpVerb::Delete) ? L"DELETE" : L"POST";

    HandleGuard req;
    req.h = WinHttpOpenRequest(conn.h, method, target_.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                               target_.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) {
        head.win32Error = GetLastError();
        Log(LogLevel::Error, "open request failed: " + std::to_string(head.win32Error));
        return head;
    }

    // Never let a redirect downgrade to plain http: the credentials attached
    // below would follow it. This is also WinHTTP's default, stated here so a
    // changed process-wide default cannot weaken it.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                     sizeof(redirectPolicy));

    if (target_.secure && cfg_.tlsIgnore != TlsIgnoreNothing) {
        DWORD secFlags = 0;
        if (cfg_.tlsIgnore & TlsIgnoreUnknownCa)    secFlags |= SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        if (cfg_.tlsIgnore & TlsIgnoreNameMismatch) secFlags |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
        if (cfg_.tlsIgnore & TlsIgnoreExpired)      secFlags |= SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        if (cfg_.tlsIgnore & TlsIgnoreWrongUsage)   secFlags |= SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req.h, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

        std::call_once(tlsWaivedWarned_, [this] {
            Log(LogLevel::Warn, "TLS checks waived: " + DescribeTlsIgnore(cfg_.tlsIgnore) +
                                    ". The connection is encrypted but the server is not "
                                    "fully verified.");
        });
    }

    if (cfg_.autologonAnyHost) {
        // Without this, WinHTTP only auto-sends the logged-in user's
        // credentials to hosts in the Intranet zone, which most corporate MCP
        // endpoints are not. That widening is gated on actually knowing who
        // the server is: otherwise the Negotiate/NTLM exchange can be
        // intercepted and relayed, so it takes an explicit opt-in.
        if (PeerIdentityVerified() || cfg_.allowInsecureAuth) {
            DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
            WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy));
        } else {
            std::call_once(insecureAuthWarned_, [this] {
                const char* why = target_.secure
                                      ? "TLS identity waived"
                                      : "plain http";
                Log(LogLevel::Warn,
                    std::string(why) +
                        ": not sending credentials outside the Intranet zone. Use a verified "
                        "https url, or pass --allow-insecure-auth if you accept the risk.");
            });
        }
    }

    std::wstring headers =
        L"Content-Type: application/json\r\n"
        L"Accept: application/json, text/event-stream\r\n";
    // Already validated where it was read, but this is the point where a bad
    // value would become injected headers, so it is checked again here.
    if (!sessionId.empty() && IsValidSessionId(sessionId))
        headers += L"Mcp-Session-Id: " + Utf8ToWide(sessionId) + L"\r\n";
    WinHttpAddRequestHeaders(req.h, headers.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD);

    const unsigned attempts = cfg_.maxAuthRetries ? cfg_.maxAuthRetries : 1;
    DWORD          status   = 0;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                static_cast<DWORD>(body.size()), 0) ||
            !WinHttpReceiveResponse(req.h, nullptr)) {
            head.win32Error = GetLastError();
            // The attempt number matters: a failure on attempt 0 is the
            // server or the network, one on a later attempt is the auth
            // handshake.
            Log(LogLevel::Error, "send failed on attempt " + std::to_string(attempt) + ": " +
                                     std::to_string(head.win32Error));
            return head;
        }

        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            head.win32Error = GetLastError();
            return head;
        }
        if (status != HTTP_STATUS_DENIED) break;

        // The challenge usually carries a body (IIS and most frameworks send
        // one). WinHTTP discards it by itself when the handle is reused, so
        // this is belt and braces rather than a fix for an observed failure;
        // it costs one call that returns zero bytes when there is nothing
        // left, and it keeps the handle in a state we can reason about.
        {
            char  sink[4096];
            DWORD drained = 0;
            while (WinHttpReadData(req.h, sink, sizeof(sink), &drained) && drained > 0) {
            }
        }

        // 401: answer as the current Windows user. NULL credentials is what
        // makes WinHTTP use the logged-in token rather than a supplied one.
        DWORD supported = 0, first = 0, authTarget = 0;
        if (!WinHttpQueryAuthSchemes(req.h, &supported, &first, &authTarget)) {
            Log(LogLevel::Error, "no usable auth scheme: " + std::to_string(GetLastError()));
            break;
        }
        DWORD scheme = 0;
        if (supported & WINHTTP_AUTH_SCHEME_NEGOTIATE)  scheme = WINHTTP_AUTH_SCHEME_NEGOTIATE;
        else if (supported & WINHTTP_AUTH_SCHEME_NTLM)  scheme = WINHTTP_AUTH_SCHEME_NTLM;
        if (!scheme) {
            Log(LogLevel::Error, "server offers neither Negotiate nor NTLM");
            break;
        }
        WinHttpSetCredentials(req.h, authTarget, scheme, nullptr, nullptr, nullptr);
    }

    head.statusCode  = status;
    head.contentType = Lower(NarrowAscii(QueryHeader(req.h, L"Content-Type")));

    const std::string rawSession = NarrowAscii(QueryHeader(req.h, L"Mcp-Session-Id"));
    if (!rawSession.empty()) {
        if (IsValidSessionId(rawSession)) {
            head.sessionId = rawSession;
        } else {
            // Refusing it costs us session continuity; accepting it would let
            // the server dictate the headers of every later request.
            Log(LogLevel::Warn,
                "ignoring a malformed Mcp-Session-Id: " + SanitizeHeaderValue(rawSession));
        }
    }
    if (onHead) onHead(head);

    if (status < 200 || status >= 300) return head;

    char chunk[8192];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(req.h, chunk, sizeof(chunk), &read)) {
            // A read error is not end of body. Reporting it as one would hand
            // the caller a truncated message and call it a success.
            head.win32Error = GetLastError();
            head.complete   = false;
            Log(LogLevel::Error, "response body cut short: " + std::to_string(head.win32Error));
            return head;
        }
        if (read == 0) break;  // end of body
        if (onChunk) onChunk(chunk, read);
    }
    return head;
}

}  // namespace

Error MakeWinHttpTransport(const Config&                cfg,
                           const ParsedUrl&             target,
                           LogSink                      log,
                           std::unique_ptr<ITransport>& out) {
    HINTERNET session = WinHttpOpen(cfg.userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        const unsigned long err = GetLastError();
        return MakeError(Status::TransportInitFailed, "WinHttpOpen failed", err);
    }
    WinHttpSetTimeouts(session, cfg.timeouts.resolveMs, cfg.timeouts.connectMs,
                       cfg.timeouts.sendMs, cfg.timeouts.receiveMs);

    out.reset(new WinHttpTransport(session, cfg, target, std::move(log)));
    return Error{};
}

}  // namespace mcpwinauth
