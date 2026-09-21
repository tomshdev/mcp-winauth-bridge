#include "mcpwinauth/framing.hpp"

#include "mcpwinauth/jsonrpc.hpp"

namespace mcpwinauth {

SseFramer::SseFramer(MessageSink out) : out_(std::move(out)) {}

void SseFramer::Feed(const char* data, size_t len) {
    buf_.append(data, len);
    size_t pos;
    while ((pos = buf_.find('\n')) != std::string::npos) {
        std::string line = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        // A \r\n split across two Feed() calls must not leave a stray \r.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ConsumeLine(std::move(line));
    }
}

void SseFramer::ConsumeLine(std::string line) {
    if (line.empty()) {  // blank line: end of one event
        Flush();
        return;
    }
    if (line.compare(0, 5, "data:") == 0) {
        const size_t start = (line.size() > 5 && line[5] == ' ') ? 6 : 5;
        if (haveData_) data_ += '\n';
        data_ += line.substr(start);
        haveData_ = true;
    }
    // id:, event:, retry: and ":" comments carry nothing we need.
}

void SseFramer::Flush() {
    if (!haveData_) return;
    if (out_ && !IsBlank(data_)) out_(ToSingleLine(data_));
    data_.clear();
    haveData_ = false;
}

void SseFramer::Finish() {
    // A stream that ends without a trailing blank line still had a last event.
    if (!buf_.empty()) {
        std::string line = std::move(buf_);
        buf_.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ConsumeLine(std::move(line));
    }
    Flush();
}

PlainBodyFramer::PlainBodyFramer(MessageSink out) : out_(std::move(out)) {}

void PlainBodyFramer::Feed(const char* data, size_t len) { buf_.append(data, len); }

void PlainBodyFramer::Finish() {
    // An empty or whitespace-only body is what a 202 for a notification looks
    // like. Emitting a blank line there would corrupt the stdio stream.
    if (out_ && !IsBlank(buf_)) out_(ToSingleLine(buf_));
    buf_.clear();
}

bool IsEventStream(const std::string& contentType) {
    return contentType.find("text/event-stream") != std::string::npos;
}

std::unique_ptr<IBodyFramer> MakeFramer(const std::string& contentType, MessageSink out) {
    if (IsEventStream(contentType))
        return std::unique_ptr<IBodyFramer>(new SseFramer(std::move(out)));
    return std::unique_ptr<IBodyFramer>(new PlainBodyFramer(std::move(out)));
}

}  // namespace mcpwinauth
