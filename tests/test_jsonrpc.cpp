#include <doctest/doctest.h>

#include "mcpwinauth/jsonrpc.hpp"

using namespace mcpwinauth;

TEST_CASE("ToSingleLine replaces every newline with a space") {
    CHECK(ToSingleLine("a\nb") == "a b");
    CHECK(ToSingleLine("a\r\nb") == "a  b");
    CHECK(ToSingleLine("") == "");
    CHECK(ToSingleLine("no newlines") == "no newlines");
}

TEST_CASE("IsBlank") {
    CHECK(IsBlank(""));
    CHECK(IsBlank("   \t\r\n"));
    CHECK_FALSE(IsBlank("{}"));
    CHECK_FALSE(IsBlank("  x  "));
}

TEST_CASE("TopLevelId finds ids of every JSON type") {
    CHECK(TopLevelId(R"({"jsonrpc":"2.0","id":7,"method":"ping"})") == "7");
    CHECK(TopLevelId(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})") == "\"abc\"");
    CHECK(TopLevelId(R"({"id":null})") == "null");
    CHECK(TopLevelId(R"({"id" : 42 })") == "42");
    CHECK(TopLevelId(R"({"id":-1})") == "-1");
    CHECK(TopLevelId(R"({"method":"ping"})").empty());
    CHECK(TopLevelId("").empty());
}

TEST_CASE("TopLevelId ignores ids that are not at the top level") {
    // Nested inside params: not the message id.
    CHECK(TopLevelId(R"({"method":"x","params":{"id":99}})").empty());
    CHECK(TopLevelId(R"({"params":{"id":99},"id":1})") == "1");
    CHECK(TopLevelId(R"({"params":[{"id":99}],"id":1})") == "1");
}

TEST_CASE("TopLevelId is not fooled by text inside string values") {
    CHECK(TopLevelId(R"({"method":"has \"id\": 5 inside","id":3})") == "3");
    CHECK(TopLevelId(R"({"note":"\"id\":9","id":4})") == "4");
    // A backslash before the closing quote must not end the string early.
    CHECK(TopLevelId(R"({"note":"ends with a backslash \\","id":8})") == "8");
}

TEST_CASE("TopLevelId gives up on malformed input instead of reading past the end") {
    CHECK(TopLevelId(R"({"id")").empty());
    CHECK(TopLevelId(R"({"id":)").empty());
    CHECK(TopLevelId(R"({"unterminated)").empty());
    CHECK(TopLevelId(R"({"a":"unterminated\)").empty());
    CHECK(TopLevelId("{").empty());
}

TEST_CASE("IsRequest requires a top-level method") {
    CHECK(IsRequest(R"({"jsonrpc":"2.0","id":1,"method":"ping"})"));
    CHECK(IsRequest(R"({"jsonrpc":"2.0","method":"notifications/x"})"));
    CHECK_FALSE(IsRequest(R"({"jsonrpc":"2.0","id":1,"result":{}})"));
    // The reference used a plain find(), which matched both of these.
    CHECK_FALSE(IsRequest(R"({"id":1,"result":{"method":"nested"}})"));
    CHECK_FALSE(IsRequest(R"({"id":1,"result":"the word \"method\" in text"})"));
}

TEST_CASE("EscapeJsonString escapes what JSON requires") {
    CHECK(EscapeJsonString("plain") == "plain");
    CHECK(EscapeJsonString("a\"b") == "a\\\"b");
    CHECK(EscapeJsonString("a\\b") == "a\\\\b");
    CHECK(EscapeJsonString("a\nb") == "a\\nb");
    CHECK(EscapeJsonString("a\tb") == "a\\tb");
    CHECK(EscapeJsonString(std::string("a\x01" "b")) == "a\\u0001b");
}

TEST_CASE("MakeErrorResponse reuses the raw id token") {
    CHECK(MakeErrorResponse("7", -32603, "boom") ==
          R"({"jsonrpc":"2.0","id":7,"error":{"code":-32603,"message":"boom"}})");
    CHECK(MakeErrorResponse("\"a\"", -1, "x") ==
          R"({"jsonrpc":"2.0","id":"a","error":{"code":-1,"message":"x"}})");
}

TEST_CASE("MakeFailureReply answers requests and stays silent otherwise") {
    SUBCASE("a request with an id gets an error back") {
        const std::string reply =
            MakeFailureReply(R"({"jsonrpc":"2.0","id":5,"method":"tools/call"})", "failed");
        CHECK(reply == R"({"jsonrpc":"2.0","id":5,"error":{"code":-32603,"message":"failed"}})");
    }
    SUBCASE("a notification gets nothing") {
        CHECK(MakeFailureReply(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "x")
                  .empty());
        CHECK(MakeFailureReply(R"({"jsonrpc":"2.0","id":null,"method":"x"})", "x").empty());
    }
    SUBCASE("a response gets nothing") {
        CHECK(MakeFailureReply(R"({"jsonrpc":"2.0","id":5,"result":{}})", "x").empty());
    }
    SUBCASE("the message is escaped into the reply") {
        const std::string reply =
            MakeFailureReply(R"({"id":1,"method":"x"})", "quote \" and newline \n");
        CHECK(reply.find("quote \\\" and newline \\n") != std::string::npos);
    }
}
