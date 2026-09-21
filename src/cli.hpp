// Pure argv -> Config translation. Never prints, never exits.
#pragma once

#include <string>
#include <vector>

#include "mcpwinauth/config.hpp"

namespace mcpwinauth {

struct CliResult {
    Config cfg;
    bool   showHelp    = false;
    bool   showVersion = false;
};

extern const char* const kVersion;

// argv without the program name.
Error ParseArgs(const std::vector<std::wstring>& args, CliResult& out);

std::string UsageText(const std::string& programName);

}  // namespace mcpwinauth
