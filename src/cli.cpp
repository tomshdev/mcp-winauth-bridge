#include "cli.hpp"

#include <cwchar>
#include <string>

namespace mcpwinauth {

const char* const kVersion = "1.0.0";

namespace {

bool ParseUnsigned(const std::wstring& text, unsigned& out) {
    if (text.empty()) return false;
    unsigned long long v = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<unsigned>(c - L'0');
        if (v > 0xFFFFFFFFull) return false;
    }
    out = static_cast<unsigned>(v);
    return true;
}

bool ParseInt(const std::wstring& text, int& out) {
    unsigned v = 0;
    if (!ParseUnsigned(text, v) || v > 0x7FFFFFFFu) return false;
    out = static_cast<int>(v);
    return true;
}

std::string Narrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t c : s) out += (c < 128) ? static_cast<char>(c) : '?';
    return out;
}

Error BadArg(const std::wstring& what) {
    return MakeError(Status::InvalidArgument, Narrow(what));
}

}  // namespace

Error ParseArgs(const std::vector<std::wstring>& args, CliResult& out) {
    out = CliResult{};
    bool haveUrl = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];

        if (a == L"-h" || a == L"--help") { out.showHelp = true; return Error{}; }
        if (a == L"--version")            { out.showVersion = true; return Error{}; }

        // Every remaining flag takes one value.
        auto value = [&](std::wstring& dest) -> bool {
            if (i + 1 >= args.size()) return false;
            dest = args[++i];
            return true;
        };

        if (a.size() > 2 && a.compare(0, 2, L"--") == 0) {
            std::wstring v;
            if (a == L"--user-agent") {
                if (!value(v)) return BadArg(a + L" needs a value");
                out.cfg.userAgent = v;
            } else if (a == L"--workers") {
                if (!value(v) || !ParseUnsigned(v, out.cfg.workerCount) || out.cfg.workerCount == 0)
                    return BadArg(a + L" needs a positive integer");
            } else if (a == L"--queue-depth") {
                if (!value(v) || !ParseUnsigned(v, out.cfg.maxQueueDepth))
                    return BadArg(a + L" needs an integer");
            } else if (a == L"--auth-retries") {
                if (!value(v) || !ParseUnsigned(v, out.cfg.maxAuthRetries) || out.cfg.maxAuthRetries == 0)
                    return BadArg(a + L" needs a positive integer");
            } else if (a == L"--resolve-timeout") {
                if (!value(v) || !ParseInt(v, out.cfg.timeouts.resolveMs))
                    return BadArg(a + L" needs milliseconds");
            } else if (a == L"--connect-timeout") {
                if (!value(v) || !ParseInt(v, out.cfg.timeouts.connectMs))
                    return BadArg(a + L" needs milliseconds");
            } else if (a == L"--send-timeout") {
                if (!value(v) || !ParseInt(v, out.cfg.timeouts.sendMs))
                    return BadArg(a + L" needs milliseconds");
            } else if (a == L"--receive-timeout") {
                if (!value(v) || !ParseInt(v, out.cfg.timeouts.receiveMs))
                    return BadArg(a + L" needs milliseconds");
            } else if (a == L"--drain-timeout") {
                if (!value(v) || !ParseUnsigned(v, out.cfg.drainTimeoutMs))
                    return BadArg(a + L" needs milliseconds");
            } else if (a == L"--log-level") {
                if (!value(v)) return BadArg(a + L" needs a level");
                if      (v == L"error") out.cfg.logLevel = LogLevel::Error;
                else if (v == L"warn")  out.cfg.logLevel = LogLevel::Warn;
                else if (v == L"info")  out.cfg.logLevel = LogLevel::Info;
                else if (v == L"debug") out.cfg.logLevel = LogLevel::Debug;
                else return BadArg(L"--log-level must be error, warn, info or debug");
            } else if (a == L"--no-autologon") {
                out.cfg.autologonAnyHost = false;
            } else if (a == L"--allow-insecure-auth") {
                out.cfg.allowInsecureAuth = true;
            } else if (a == L"--no-delete-session") {
                out.cfg.deleteSessionOnShutdown = false;
            } else {
                return BadArg(L"unknown option " + a);
            }
            continue;
        }

        if (haveUrl) return BadArg(L"unexpected extra argument " + a);
        out.cfg.url = a;
        haveUrl     = true;
    }

    if (!haveUrl) return MakeError(Status::InvalidArgument, "missing <mcp-url>");
    return Error{};
}

std::string UsageText(const std::string& programName) {
    return
        "usage: " + programName + " [options] <mcp-url>\n"
        "\n"
        "Bridges stdio MCP to a Streamable HTTP MCP server using Windows\n"
        "integrated auth (Negotiate / Kerberos / NTLM) as the logged-in user.\n"
        "One JSON-RPC message per line on stdin and stdout; logs on stderr.\n"
        "\n"
        "Options:\n"
        "  --user-agent <s>         User-Agent header (default mcp-winauth-bridge/1.0)\n"
        "  --workers <n>            Concurrent in-flight requests (default 4)\n"
        "  --queue-depth <n>        Pending messages before stdin blocks, 0 = unbounded (default 256)\n"
        "  --auth-retries <n>       Attempts in the 401 auth loop (default 3)\n"
        "  --resolve-timeout <ms>   DNS timeout (default 10000)\n"
        "  --connect-timeout <ms>   TCP connect timeout (default 10000)\n"
        "  --send-timeout <ms>      Send timeout (default 30000)\n"
        "  --receive-timeout <ms>   Receive timeout (default 600000)\n"
        "  --drain-timeout <ms>     Wait for in-flight requests at exit, 0 = forever (default 0)\n"
        "  --log-level <l>          error | warn | info | debug (default info)\n"
        "  --no-autologon           Do not send credentials to non-intranet hosts\n"
        "  --allow-insecure-auth    Allow Windows auth over plain http (exposes the exchange)\n"
        "  --no-delete-session      Skip the DELETE that ends the server session\n"
        "  -h, --help               Show this help\n"
        "      --version            Show the version\n";
}

}  // namespace mcpwinauth
