#include <doctest/doctest.h>

#include <string>

#include "mcpwinauth/headers.hpp"

using namespace mcpwinauth;

TEST_CASE("IsValidSessionId accepts what the spec allows") {
    CHECK(IsValidSessionId("sess-abc123"));
    CHECK(IsValidSessionId("1234-5678-90ab-cdef"));
    CHECK(IsValidSessionId("A"));
    // Every visible ASCII character is legal, punctuation included.
    CHECK(IsValidSessionId("!\"#$%&'()*+,-./:;<=>?@[]^_`{|}~"));
}

TEST_CASE("IsValidSessionId rejects anything that could inject a header") {
    // These are the whole point: the value is concatenated into a CRLF
    // delimited header block on every later request.
    CHECK_FALSE(IsValidSessionId("abc\r\nX-Injected: 1"));
    CHECK_FALSE(IsValidSessionId("abc\rX: 1"));
    CHECK_FALSE(IsValidSessionId("abc\nX: 1"));
    CHECK_FALSE(IsValidSessionId(std::string("abc\0def", 7)));
}

TEST_CASE("IsValidSessionId rejects empty, spaced and oversized values") {
    CHECK_FALSE(IsValidSessionId(""));
    CHECK_FALSE(IsValidSessionId("has space"));
    CHECK_FALSE(IsValidSessionId("trailing "));
    CHECK_FALSE(IsValidSessionId("\ttab"));
    CHECK(IsValidSessionId(std::string(512, 'a')));
    CHECK_FALSE(IsValidSessionId(std::string(513, 'a')));
}

TEST_CASE("IsValidSessionId rejects non-ASCII") {
    CHECK_FALSE(IsValidSessionId("caf\xc3\xa9"));
    CHECK_FALSE(IsValidSessionId(std::string(1, static_cast<char>(0x7F))));
}

TEST_CASE("SanitizeHeaderValue keeps text readable and strips control characters") {
    CHECK(SanitizeHeaderValue("plain value") == "plain value");
    CHECK(SanitizeHeaderValue("a\r\nb") == "a??b");
    CHECK(SanitizeHeaderValue("a\tb") == "a\tb");
    CHECK(SanitizeHeaderValue(std::string("a\0b", 3)) == "a?b");
    CHECK(SanitizeHeaderValue("") == "");
}
