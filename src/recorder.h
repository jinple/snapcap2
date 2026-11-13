// Recording of one window to an MP4 file: H.264 video at 60 fps from Windows
// Graphics Capture (or Desktop Duplication when a fullscreen window gives
// window capture nothing), AAC audio from that window's process tree only.
#pragma once
#include <windows.h>
#include <string>

namespace rec {

enum State { Idle, Starting, Recording, Stopping };

struct Result {
    bool ok = false;
    std::wstring file;      // where the MP4 was written
    std::wstring error;     // why it failed, if it did
    bool audio = false;     // application audio was captured
    bool screenMethod = false; // Desktop Duplication was used
    bool hwEncoder = false;
    long long frames = 0, dropped = 0; // dropped: no free buffer or drawing failed
    long long late = 0;                // frame times skipped because the recorder's own loop fell behind
    double seconds = 0;
    double startUnix = 0;   // when the recording's clock started (for tests)
    int width = 0, height = 0;
    int switches = 0;                       // times the recording changed capture method
    int probeWindow = -1, probeScreen = -1; // frames each capture method delivered in the fullscreen check
};

// Recording settings, read when a recording starts.
struct Options {
    int fps = 60;            // 30 or 60
    int maxHeight = 0;       // 0 = the window's own size; otherwise the video is scaled down to this height
    int bitrateMbps = 0;     // 0 = chosen from the video's size
    int corner = 0;          // where "Show in video" draws: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    std::wstring folder;     // empty = Videos\SnapCap
};
void setOptions(const Options& o);

// Starts recording `target`. Returns false if a recording is already running.
// When the file is finished (stopped, failed, or the window went away),
// `doneMsg` is posted to `notify`; call result() then.
bool start(HWND target, DWORD pid, const std::wstring& name, HWND notify, UINT doneMsg);
void stop();               // asks the recording to finish; returns at once
void shutdown();           // stop and wait for the file to be finished
State state();
Result result();

void setOverlayText(const std::wstring& name, const std::wstring& fps); // what "Show in video" draws
void setShowInVideo(bool show);
double arrivalFps();       // frames per second reaching the recorder (capped FPS source while recording)

}
