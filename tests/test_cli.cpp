#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli.hpp"

using namespace mcpwinauth;

namespace {

CliResult MustParse(const std::vector<std::wstring>& args) {
    CliResult out;
    const Error e = ParseArgs(args, out);
    REQUIRE_MESSAGE(static_cast<bool>(e), e.message);
    return out;
}

Error ParseFails(const std::vector<std::wstring>& args) {
    CliResult   out;
    const Error e = ParseArgs(args, out);
    CHECK_FALSE(static_cast<bool>(e));
    return e;
}

}  // namespace

TEST_CASE("the url is the one positional argument") {
    const CliResult r = MustParse({L"https://server/mcp"});
    CHECK(r.cfg.url == L"https://server/mcp");
    CHECK_FALSE(r.showHelp);
    CHECK_FALSE(r.showVersion);
}

TEST_CASE("defaults match the documented ones") {
    const CliResult r = MustParse({L"https://server/mcp"});
    CHECK(r.cfg.workerCount == 4);
    CHECK(r.cfg.maxQueueDepth == 256);
    CHECK(r.cfg.maxAuthRetries == 3);
    CHECK(r.cfg.timeouts.receiveMs == 600000);
    CHECK(r.cfg.drainTimeoutMs == 0);
    CHECK(r.cfg.autologonAnyHost);
    CHECK(r.cfg.deleteSessionOnShutdown);
    CHECK(r.cfg.logLevel == LogLevel::Info);
}

TEST_CASE("help and version short-circuit, even without a url") {
    CHECK(MustParse({L"--help"}).showHelp);
    CHECK(MustParse({L"-h"}).showHelp);
    CHECK(MustParse({L"--version"}).showVersion);
    CHECK(MustParse({L"https://server/mcp", L"--help"}).showHelp);
}

TEST_CASE("numeric options are parsed") {
    const CliResult r = MustParse({L"--workers", L"8", L"--queue-depth", L"0",
                                   L"--auth-retries", L"5", L"--receive-timeout", L"1234",
                                   L"--drain-timeout", L"5000", L"https://server/mcp"});
    CHECK(r.cfg.workerCount == 8);
    CHECK(r.cfg.maxQueueDepth == 0);
    CHECK(r.cfg.maxAuthRetries == 5);
    CHECK(r.cfg.timeouts.receiveMs == 1234);
    CHECK(r.cfg.drainTimeoutMs == 5000);
}

TEST_CASE("every timeout has its own flag") {
    const CliResult r = MustParse({L"--resolve-timeout", L"1", L"--connect-timeout", L"2",
                                   L"--send-timeout", L"3", L"--receive-timeout", L"4",
                                   L"https://server/mcp"});
    CHECK(r.cfg.timeouts.resolveMs == 1);
    CHECK(r.cfg.timeouts.connectMs == 2);
    CHECK(r.cfg.timeouts.sendMs == 3);
    CHECK(r.cfg.timeouts.receiveMs == 4);
}

TEST_CASE("log levels") {
    CHECK(MustParse({L"--log-level", L"error", L"u"}).cfg.logLevel == LogLevel::Error);
    CHECK(MustParse({L"--log-level", L"warn", L"u"}).cfg.logLevel == LogLevel::Warn);
    CHECK(MustParse({L"--log-level", L"info", L"u"}).cfg.logLevel == LogLevel::Info);
    CHECK(MustParse({L"--log-level", L"debug", L"u"}).cfg.logLevel == LogLevel::Debug);
    ParseFails({L"--log-level", L"verbose", L"u"});
}

TEST_CASE("boolean flags take no value") {
    const CliResult r = MustParse({L"--no-autologon", L"--no-delete-session",
                                   L"--allow-insecure-auth", L"https://server/mcp"});
    CHECK_FALSE(r.cfg.autologonAnyHost);
    CHECK_FALSE(r.cfg.deleteSessionOnShutdown);
    CHECK(r.cfg.allowInsecureAuth);
    CHECK(r.cfg.url == L"https://server/mcp");
}

TEST_CASE("TLS checks are all enforced by default") {
    CHECK(MustParse({L"https://server/mcp"}).cfg.tlsIgnore == TlsIgnoreNothing);
}

TEST_CASE("--insecure and -k waive every check") {
    CHECK(MustParse({L"--insecure", L"u"}).cfg.tlsIgnore == TlsIgnoreEverything);
    CHECK(MustParse({L"-k", L"u"}).cfg.tlsIgnore == TlsIgnoreEverything);
    // -k is two characters, so it must be matched before the "--" branch or
    // it gets mistaken for the url.
    CHECK(MustParse({L"-k", L"https://server/mcp"}).cfg.url == L"https://server/mcp");
}

TEST_CASE("--tls-ignore waives only what it names") {
    CHECK(MustParse({L"--tls-ignore", L"unknown-ca", L"u"}).cfg.tlsIgnore == TlsIgnoreUnknownCa);
    CHECK(MustParse({L"--tls-ignore", L"name", L"u"}).cfg.tlsIgnore == TlsIgnoreNameMismatch);
    CHECK(MustParse({L"--tls-ignore", L"expired", L"u"}).cfg.tlsIgnore == TlsIgnoreExpired);
    CHECK(MustParse({L"--tls-ignore", L"usage", L"u"}).cfg.tlsIgnore == TlsIgnoreWrongUsage);
    CHECK(MustParse({L"--tls-ignore", L"all", L"u"}).cfg.tlsIgnore == TlsIgnoreEverything);
}

TEST_CASE("--tls-ignore takes a comma separated list and accumulates") {
    // The case that matters: an https://<ip>/ url with a private CA needs the
    // name check waived but should still require a trusted chain... and here
    // the caller asked for both.
    const unsigned both = TlsIgnoreUnknownCa | TlsIgnoreNameMismatch;
    CHECK(MustParse({L"--tls-ignore", L"unknown-ca,name", L"u"}).cfg.tlsIgnore == both);
    CHECK(MustParse({L"--tls-ignore", L"name,unknown-ca", L"u"}).cfg.tlsIgnore == both);
    CHECK(MustParse({L"--tls-ignore", L"name", L"--tls-ignore", L"unknown-ca", L"u"}).cfg.tlsIgnore == both);
    CHECK(MustParse({L"--tls-ignore", L"name,name", L"u"}).cfg.tlsIgnore == TlsIgnoreNameMismatch);
}

TEST_CASE("--tls-ignore rejects nonsense") {
    ParseFails({L"--tls-ignore", L"u"});                  // consumes the url, none left
    ParseFails({L"--tls-ignore", L"bogus", L"u"});
    ParseFails({L"--tls-ignore", L"name,bogus", L"u"});
    ParseFails({L"--tls-ignore", L"", L"u"});
    ParseFails({L"--tls-ignore", L"name,", L"u"});
    ParseFails({L"https://server/mcp", L"--tls-ignore"}); // missing value
}

TEST_CASE("only the CA and name waivers defeat the server's identity") {
    // Which ones count decides whether credentials may be widened beyond the
    // Intranet zone, so it is worth pinning down.
    CHECK((TlsIgnoreUnknownCa    & kTlsIgnoreDefeatsIdentity) != 0);
    CHECK((TlsIgnoreNameMismatch & kTlsIgnoreDefeatsIdentity) != 0);
    // An expired or wrong-usage cert still chains to a trusted CA for the
    // right name, so it still says who the peer is.
    CHECK((TlsIgnoreExpired      & kTlsIgnoreDefeatsIdentity) == 0);
    CHECK((TlsIgnoreWrongUsage   & kTlsIgnoreDefeatsIdentity) == 0);
}

TEST_CASE("DescribeTlsIgnore names the waived checks") {
    CHECK(DescribeTlsIgnore(TlsIgnoreNothing) == "nothing");
    CHECK(DescribeTlsIgnore(TlsIgnoreUnknownCa) == "untrusted CA");
    CHECK(DescribeTlsIgnore(TlsIgnoreUnknownCa | TlsIgnoreNameMismatch) ==
          "untrusted CA, name mismatch");
    CHECK(DescribeTlsIgnore(TlsIgnoreEverything) ==
          "untrusted CA, name mismatch, expiry, key usage");
}

TEST_CASE("credentials over plain http need an explicit opt-in") {
    // Sending a Negotiate/NTLM exchange over http exposes it to the path, so
    // the default must not be to do it silently.
    CHECK_FALSE(MustParse({L"http://server/mcp"}).cfg.allowInsecureAuth);
    CHECK(MustParse({L"--allow-insecure-auth", L"http://server/mcp"}).cfg.allowInsecureAuth);
}

TEST_CASE("the user agent is taken verbatim") {
    CHECK(MustParse({L"--user-agent", L"my-host/2.0", L"u"}).cfg.userAgent == L"my-host/2.0");
}

TEST_CASE("bad input is rejected without printing or exiting") {
    SUBCASE("no url")            { ParseFails({}); }
    SUBCASE("unknown option")    { ParseFails({L"--nope", L"u"}); }
    SUBCASE("missing value")     { ParseFails({L"https://server/mcp", L"--workers"}); }
    SUBCASE("not a number")      { ParseFails({L"--workers", L"many", L"u"}); }
    SUBCASE("zero workers")      { ParseFails({L"--workers", L"0", L"u"}); }
    SUBCASE("zero auth retries") { ParseFails({L"--auth-retries", L"0", L"u"}); }
    SUBCASE("two urls")          { ParseFails({L"https://a/mcp", L"https://b/mcp"}); }
    SUBCASE("overflow")          { ParseFails({L"--workers", L"99999999999999", L"u"}); }
}

TEST_CASE("usage text names the program and every option") {
    const std::string usage = UsageText("mcp-winauth-bridge");
    CHECK(usage.find("mcp-winauth-bridge") != std::string::npos);
    for (const char* flag : {"--workers", "--queue-depth", "--log-level", "--drain-timeout",
                             "--no-autologon", "--allow-insecure-auth", "--no-delete-session",
                             "--insecure", "--tls-ignore", "unknown-ca", "--version"}) {
        CAPTURE(flag);
        CHECK(usage.find(flag) != std::string::npos);
    }
}
