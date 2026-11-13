// Test helper: plays a quiet sine tone from its own process for a few seconds.
// Stands in for "other sound on the computer" that a recording must not contain.
//   tone.exe <hz> <seconds>
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

int wmain(int argc, wchar_t** argv) {
    int hz = argc > 1 ? _wtoi(argv[1]) : 3000;
    double seconds = argc > 2 ? _wtof(argv[2]) : 5;
    const int rate = 48000, n = rate;
    struct { char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmtSize; uint16_t tag, ch; uint32_t rate, bytes; uint16_t align, bits; char data[4]; uint32_t dataSize; } h =
        {{'R','I','F','F'}, 36 + n * 4, {'W','A','V','E'}, {'f','m','t',' '}, 16, 1, 2, rate, rate * 4, 4, 16, {'d','a','t','a'}, uint32_t(n * 4)};
    std::string wav((const char*)&h, sizeof(h));
    for (int i = 0; i < n; i++) {
        int16_t v = int16_t(0.08 * 32767.0 * sin(2.0 * 3.14159265358979 * hz * i / rate));
        wav.append((const char*)&v, 2);
        wav.append((const char*)&v, 2);
    }
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    Sleep(DWORD(seconds * 1000));
    PlaySoundW(nullptr, nullptr, 0);
    return 0;
}
