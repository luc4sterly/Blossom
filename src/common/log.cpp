#include <cstdlib>
#include "common/log.hpp"

bool Log::stdout_enabled = [] {
    const char* env = std::getenv("BLOSSOM_LOG");
    return env && env[0] && env[0] != '0';
}();
