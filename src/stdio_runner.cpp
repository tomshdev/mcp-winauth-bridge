#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <istream>
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "mcpwinauth/bridge.hpp"

namespace mcpwinauth {

Error RunStdioBridge(Config cfg, std::istream& in, MessageSink out, LogSink log) {
    Bridge bridge(std::move(cfg), std::move(out), std::move(log));

    Error e = bridge.Start();
    if (!e) return e;

    std::string line;
    while (std::getline(in, line)) {
        // Tolerate CRLF: a stdio client on Windows may send either.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!bridge.Submit(std::move(line))) break;
        line.clear();
    }

    bridge.Shutdown();
    return Error{};
}

int BridgeMain(int argc, wchar_t** argv) {
    const std::string programName = "mcp-winauth-bridge";

    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    CliResult parsed;
    Error     e = ParseArgs(args, parsed);

    if (parsed.showHelp) {
        std::fputs(UsageText(programName).c_str(), stdout);
        return 0;
    }
    if (parsed.showVersion) {
        std::printf("%s %s\n", programName.c_str(), kVersion);
        return 0;
    }
    if (!e) {
        std::fprintf(stderr, "%s: %s\n\n", programName.c_str(), e.message.c_str());
        std::fputs(UsageText(programName).c_str(), stderr);
        return 2;
    }

    // Binary mode: the CRT must not rewrite \n as \r\n on the way out, or
    // translate on the way in. Each JSON-RPC message is one line exactly.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    LogSink log = MakeStderrLogSink(parsed.cfg.logLevel);

    e = RunStdioBridge(parsed.cfg, std::cin, MakeStdoutLineSink(), log);
    if (!e) {
        std::string text = e.message;
        if (e.win32) text += " (win32 " + std::to_string(e.win32) + ")";
        log(LogLevel::Error, text);
        return 1;
    }
    return 0;
}

}  // namespace mcpwinauth
