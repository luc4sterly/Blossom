#ifndef BLOSSOM_LOG_HPP
#define BLOSSOM_LOG_HPP
//The core inherits hundreds of debug printf calls from Corgi3DS (EMMC, NDMA, AES, PXI, SVC...).
//Formatting and writing them cost a large part of each frame, so by default they are dropped
//before formatting. BLOSSOM_LOG=1 brings them back. Include this header after the system headers;
//it turns printf(...) in the including file into a checked call.
#include <cstdio>

namespace Log
{
//Set from BLOSSOM_LOG at startup; test runners turn it on.
extern bool stdout_enabled;
}

#define printf(...) (::Log::stdout_enabled ? ::printf(__VA_ARGS__) : 0)

#endif // BLOSSOM_LOG_HPP
