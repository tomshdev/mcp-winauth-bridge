#include <doctest/doctest.h>

#include <string>

#include "mcpwinauth/url.hpp"

using namespace mcpwinauth;

namespace {

ParsedUrl MustParse(const std::wstring& url) {
    ParsedUrl     out;
    unsigned long win32 = 0;
    REQUIRE(ParseUrl(url, out, win32));
    return out;
}

}  // namespace

TEST_CASE("ParseUrl splits an https url") {
    const ParsedUrl u = MustParse(L"https://grafana-mcp.corp.local/mcp");
    CHECK(u.host == L"grafana-mcp.corp.local");
    CHECK(u.path == L"/mcp");
    CHECK(u.port == 443);
    CHECK(u.secure);
}

TEST_CASE("ParseUrl splits an http url and its default port") {
    const ParsedUrl u = MustParse(L"http://server/mcp");
    CHECK(u.host == L"server");
    CHECK(u.port == 80);
    CHECK_FALSE(u.secure);
}

TEST_CASE("ParseUrl honours an explicit port") {
    CHECK(MustParse(L"http://server:8080/mcp").port == 8080);
    CHECK(MustParse(L"https://server:8443/mcp").port == 8443);
}

TEST_CASE("ParseUrl keeps the query string on the path") {
    const ParsedUrl u = MustParse(L"https://server/mcp?tenant=a&x=1");
    CHECK(u.path == L"/mcp?tenant=a&x=1");
}

TEST_CASE("ParseUrl defaults an empty path to /") {
    CHECK(MustParse(L"https://server").path == L"/");
    CHECK(MustParse(L"https://server/").path == L"/");
}

TEST_CASE("ParseUrl does not truncate a long path or a long host") {
    // The reference used fixed 256/2048 stack buffers, which silently cut
    // these short and produced a request to the wrong resource.
    SUBCASE("long path") {
        const std::wstring segment(4000, L'a');
        const ParsedUrl    u = MustParse(L"https://server/" + segment);
        CHECK(u.path.size() == segment.size() + 1);
        CHECK(u.path == L"/" + segment);
    }
    SUBCASE("long host") {
        std::wstring host;
        for (int i = 0; i < 40; ++i) host += L"segment-that-is-long.";
        host += L"local";
        const ParsedUrl u = MustParse(L"https://" + host + L"/mcp");
        CHECK(u.host == host);
    }
}

TEST_CASE("ParseUrl rejects what it cannot use") {
    ParsedUrl     out;
    unsigned long win32 = 0;

    CHECK_FALSE(ParseUrl(L"", out, win32));
    CHECK(win32 != 0);

    CHECK_FALSE(ParseUrl(L"not a url", out, win32));
    CHECK_FALSE(ParseUrl(L"ftp://server/mcp", out, win32));
    CHECK_FALSE(ParseUrl(L"file:///c:/x", out, win32));
}
