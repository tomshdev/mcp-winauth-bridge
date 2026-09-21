// A host application embedding the bridge instead of shelling out to the exe.
// It supplies its own sinks, so replies and logs go wherever it wants.
#include <cstdio>
#include <string>

#include "mcpwinauth/bridge.hpp"

int main() {
    mcpwinauth::Config cfg;
    cfg.url         = L"https://mcp.example.invalid/mcp";
    cfg.workerCount = 2;

    auto out = [](const std::string& message) {
        std::printf("reply: %s\n", message.c_str());
    };
    auto log = [](mcpwinauth::LogLevel level, const std::string& text) {
        std::fprintf(stderr, "[%s] %s\n", mcpwinauth::ToString(level), text.c_str());
    };

    mcpwinauth::Bridge bridge(cfg, out, log);

    // Not started here: this example only has to compile and link, and there
    // is no MCP server to reach from CI.
    (void)bridge.SessionId();
    std::puts("linked against mcpwinauth::bridge");
    return 0;
}
