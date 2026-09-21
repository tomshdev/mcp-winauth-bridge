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
    const CliResult r =
        MustParse({L"--no-autologon", L"--no-delete-session", L"https://server/mcp"});
    CHECK_FALSE(r.cfg.autologonAnyHost);
    CHECK_FALSE(r.cfg.deleteSessionOnShutdown);
    CHECK(r.cfg.url == L"https://server/mcp");
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
                             "--no-autologon", "--no-delete-session", "--version"}) {
        CAPTURE(flag);
        CHECK(usage.find(flag) != std::string::npos);
    }
}
