#include <cstdio>
#include <memory>
#include <mutex>

#include "mcpwinauth/config.hpp"
#include "mcpwinauth/jsonrpc.hpp"

namespace mcpwinauth {

Error MakeError(Status status, std::string message, unsigned long win32) {
    Error e;
    e.status  = status;
    e.win32   = win32;
    e.message = std::move(message);
    return e;
}

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "error";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Info:  return "info";
        case LogLevel::Debug: return "debug";
    }
    return "?";
}

MessageSink MakeStdoutLineSink() {
    auto mx = std::make_shared<std::mutex>();
    return [mx](const std::string& message) {
        const std::string line = ToSingleLine(message);
        std::lock_guard<std::mutex> lock(*mx);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
}

LogSink MakeStderrLogSink(LogLevel minLevel) {
    auto mx = std::make_shared<std::mutex>();
    return [mx, minLevel](LogLevel level, const std::string& text) {
        if (static_cast<int>(level) > static_cast<int>(minLevel)) return;
        std::lock_guard<std::mutex> lock(*mx);
        std::fprintf(stderr, "[bridge] %s: %s\n", ToString(level), text.c_str());
        std::fflush(stderr);
    };
}

}  // namespace mcpwinauth
