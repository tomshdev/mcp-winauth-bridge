// The only file in the project that talks to the network. It contains no
// parsing: the response body is handed to the caller as raw chunks.
#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>

#include "mcpwinauth/transport.hpp"

#pragma comment(lib, "winhttp.lib")

namespace mcpwinauth {
namespace {

std::string NarrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t c : s) out += (c < 128) ? static_cast<char>(c) : '?';
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

    HINTERNET Session() {
        std::lock_guard<std::mutex> lock(mx_);
        return session_;
    }

    mutable std::mutex mx_;
    HINTERNET          session_ = nullptr;
    Config             cfg_;
    ParsedUrl          target_;
    LogSink            log_;
};

ResponseHead WinHttpTransport::Request(HttpVerb           verb,
                                       const std::string& body,
                                       const std::string& sessionId,
                                       const HeadFn&      onHead,
                                       const BodyChunkFn& onChunk) {
    ResponseHead head;

    HINTERNET session = Session();
    if (!session) {
        head.win32Error = ERROR_WINHTTP_OPERATION_CANCELLED;
        return head;
    }

    HandleGuard conn;
    conn.h = WinHttpConnect(session, target_.host.c_str(), target_.port, 0);
    if (!conn.h) {
        head.win32Error = GetLastError();
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

    if (cfg_.autologonAnyHost) {
        // Without this, WinHTTP only auto-sends the logged-in user's
        // credentials to hosts in the Intranet zone.
        DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
        WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy));
    }

    std::wstring headers =
        L"Content-Type: application/json\r\n"
        L"Accept: application/json, text/event-stream\r\n";
    if (!sessionId.empty()) headers += L"Mcp-Session-Id: " + Utf8ToWide(sessionId) + L"\r\n";
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
            Log(LogLevel::Error, "send failed: " + std::to_string(head.win32Error));
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
    head.sessionId   = NarrowAscii(QueryHeader(req.h, L"Mcp-Session-Id"));
    if (onHead) onHead(head);

    if (status < 200 || status >= 300) return head;

    char  chunk[8192];
    DWORD read = 0;
    while (WinHttpReadData(req.h, chunk, sizeof(chunk), &read) && read > 0) {
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
