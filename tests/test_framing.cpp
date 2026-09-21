#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "mcpwinauth/framing.hpp"

using namespace mcpwinauth;

namespace {

struct Collector {
    std::vector<std::string> messages;

    MessageSink Sink() {
        return [this](const std::string& m) { messages.push_back(m); };
    }
};

void FeedAll(IBodyFramer& f, const std::string& s) {
    f.Feed(s.data(), s.size());
}

// Feeding one byte at a time is the harshest chunking a framer can see, and it
// catches essentially every buffering mistake.
std::vector<std::string> FrameByteByByte(const std::string& body, const std::string& contentType) {
    Collector c;
    auto      f = MakeFramer(contentType, c.Sink());
    for (const char ch : body) f->Feed(&ch, 1);
    f->Finish();
    return c.messages;
}

}  // namespace

TEST_CASE("MakeFramer picks by content type") {
    Collector c;
    CHECK(IsEventStream("text/event-stream"));
    CHECK(IsEventStream("text/event-stream; charset=utf-8"));
    CHECK_FALSE(IsEventStream("application/json"));
    CHECK_FALSE(IsEventStream(""));
}

TEST_CASE("PlainBodyFramer emits the whole body once") {
    Collector c;
    PlainBodyFramer f(c.Sink());
    FeedAll(f, R"({"jsonrpc":"2.0","id":1,)");
    FeedAll(f, R"("result":{}})");
    CHECK(c.messages.empty());  // nothing until Finish
    f.Finish();
    REQUIRE(c.messages.size() == 1);
    CHECK(c.messages[0] == R"({"jsonrpc":"2.0","id":1,"result":{}})");
}

TEST_CASE("PlainBodyFramer stays silent on an empty or blank body") {
    // What an HTTP 202 for a notification looks like. Emitting a blank line
    // there would corrupt the stdio stream.
    SUBCASE("empty") {
        Collector c;
        PlainBodyFramer f(c.Sink());
        f.Finish();
        CHECK(c.messages.empty());
    }
    SUBCASE("whitespace only") {
        Collector c;
        PlainBodyFramer f(c.Sink());
        FeedAll(f, "  \r\n\t ");
        f.Finish();
        CHECK(c.messages.empty());
    }
}

TEST_CASE("PlainBodyFramer flattens a pretty-printed body to one line") {
    Collector c;
    PlainBodyFramer f(c.Sink());
    FeedAll(f, "{\n  \"id\": 1\n}");
    f.Finish();
    REQUIRE(c.messages.size() == 1);
    CHECK(c.messages[0].find('\n') == std::string::npos);
}

TEST_CASE("SseFramer emits one message per event") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, "data: {\"a\":1}\n\ndata: {\"b\":2}\n\n");
    REQUIRE(c.messages.size() == 2);
    CHECK(c.messages[0] == R"({"a":1})");
    CHECK(c.messages[1] == R"({"b":2})");
}

TEST_CASE("SseFramer survives being fed across arbitrary chunk boundaries") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, "da");
    FeedAll(f, "ta: {\"a\":1}\n");
    CHECK(c.messages.empty());  // the event is not over yet
    FeedAll(f, "\n");
    REQUIRE(c.messages.size() == 1);
    CHECK(c.messages[0] == R"({"a":1})");
}

TEST_CASE("SseFramer handles a CRLF split across two Feed calls") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, "data: {\"a\":1}\r");
    FeedAll(f, "\n\r\n");
    REQUIRE(c.messages.size() == 1);
    // A stray \r would ride along into the emitted message.
    CHECK(c.messages[0] == R"({"a":1})");
}

TEST_CASE("SseFramer accepts data: with and without the optional space") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, "data:{\"a\":1}\n\n");
    REQUIRE(c.messages.size() == 1);
    CHECK(c.messages[0] == R"({"a":1})");
}

TEST_CASE("SseFramer joins multiple data lines, then flattens them") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, "data: {\"a\":\ndata: 1}\n\n");
    REQUIRE(c.messages.size() == 1);
    // Joined with \n per the SSE spec, then single-lined for stdio.
    CHECK(c.messages[0] == "{\"a\": 1}");
    CHECK(c.messages[0].find('\n') == std::string::npos);
}

TEST_CASE("SseFramer ignores the fields it has no use for") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, ": a comment\n"
               "event: message\n"
               "id: 42\n"
               "retry: 1000\n"
               "data: {\"a\":1}\n"
               "\n");
    REQUIRE(c.messages.size() == 1);
    CHECK(c.messages[0] == R"({"a":1})");
}

TEST_CASE("SseFramer emits nothing for an event that carried no data") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, ": keepalive\n\n");
    f.Finish();
    CHECK(c.messages.empty());
}

TEST_CASE("SseFramer flushes a last event that had no trailing blank line") {
    Collector c;
    SseFramer f(c.Sink());
    FeedAll(f, "data: {\"a\":1}");
    CHECK(c.messages.empty());
    f.Finish();
    REQUIRE(c.messages.size() == 1);
    CHECK(c.messages[0] == R"({"a":1})");
}

TEST_CASE("framers give the same result one byte at a time as in one go") {
    SUBCASE("sse") {
        const std::string body =
            ": hello\n"
            "event: message\n"
            "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"x\":[1,2]}}\r\n"
            "\r\n"
            "id: 7\n"
            "data: {\"jsonrpc\":\"2.0\",\"method\":\"n\"}\n"
            "\n"
            "data: trailing-no-blank-line";
        const auto got = FrameByteByByte(body, "text/event-stream");
        REQUIRE(got.size() == 3);
        CHECK(got[0] == R"({"jsonrpc":"2.0","id":1,"result":{"x":[1,2]}})");
        CHECK(got[1] == R"({"jsonrpc":"2.0","method":"n"})");
        CHECK(got[2] == "trailing-no-blank-line");
    }
    SUBCASE("json") {
        const auto got = FrameByteByByte(R"({"id":1,"result":{}})", "application/json");
        REQUIRE(got.size() == 1);
        CHECK(got[0] == R"({"id":1,"result":{}})");
    }
}
