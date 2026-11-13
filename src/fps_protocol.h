// What the elevated FPS helper sends to snapcap.exe, one message every 250 ms
// over a one-way named pipe: a header, then one entry per presenting process.
// The helper never reads anything from the pipe.
#pragma once
#include <cstdint>

namespace fpsproto {
constexpr uint32_t kMagic = 0x50464353; // "SCFP"
constexpr uint32_t kMaxEntries = 512;
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\snapcap-fps-";
#pragma pack(push, 1)
struct Header { uint32_t magic; uint32_t count; };
struct Entry { uint32_t pid; uint32_t fpsMilli; }; // frames per second x 1000
#pragma pack(pop)
}
