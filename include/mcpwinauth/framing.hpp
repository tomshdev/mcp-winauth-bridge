// Incremental response-body framers. Pure: they turn arbitrary byte chunks into
// complete JSON-RPC messages on a sink, with no knowledge of HTTP.
#pragma once

#include <memory>
#include <string>

#include "mcpwinauth/config.hpp"

namespace mcpwinauth {

class IBodyFramer {
public:
    virtual ~IBodyFramer() = default;

    // Feed the next bytes. Any chunking is valid, including one byte at a time.
    virtual void Feed(const char* data, size_t len) = 0;

    // End of body: flush anything still buffered.
    virtual void Finish() = 0;
};

// text/event-stream. Accumulates "data:" lines and emits one message per blank
// line. "id:", "event:", "retry:" and comment lines are ignored.
class SseFramer final : public IBodyFramer {
public:
    explicit SseFramer(MessageSink out);
    void Feed(const char* data, size_t len) override;
    void Finish() override;

private:
    void ConsumeLine(std::string line);
    void Flush();

    MessageSink out_;
    std::string buf_;
    std::string data_;
    bool        haveData_ = false;
};

// application/json and anything else: buffer the whole body, emit once.
class PlainBodyFramer final : public IBodyFramer {
public:
    explicit PlainBodyFramer(MessageSink out);
    void Feed(const char* data, size_t len) override;
    void Finish() override;

private:
    MessageSink out_;
    std::string buf_;
};

// True if the Content-Type names an SSE stream.
bool IsEventStream(const std::string& contentType);

std::unique_ptr<IBodyFramer> MakeFramer(const std::string& contentType, MessageSink out);

}  // namespace mcpwinauth
