// snapcap.exe: tray program that attaches to one window the user picks and
// shows its name and frame rate in a small click-through overlay.
// Never runs elevated. Exact FPS comes from snapcap-fps.exe (elevated helper);
// if that is declined, FPS is counted from frames reaching the screen (capped
// at the monitor's refresh rate).
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <commctrl.h>
// Modern look for the standard controls in the settings window.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <d3d11.h>
#include <dxgi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "fps_protocol.h"
#include "recorder.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

// ---------------------------------------------------------------- constants
enum : UINT { WM_TRAY = WM_APP + 1, WM_HELPER_CHANGED = WM_APP + 2, WM_RECORD_DONE = WM_APP + 3 };
enum { HK_ATTACH = 1, HK_SHOW = 2, HK_RECORD = 3, HK_VIDEO = 4 };
enum { TIMER_TICK = 1 };
enum : UINT {
    CMD_DETACH = 1, CMD_SHOW, CMD_ASK_ADMIN, CMD_START_HELPER, CMD_EXIT, CMD_RECORD, CMD_VIDEO, CMD_SETTINGS, CMD_FOLDER, CMD_FOLLOW,
    CMD_WINDOW_FIRST = 1000, CMD_WINDOW_LAST = 1999
};
enum HelperState { H_OFF, H_STARTING, H_ON, H_DECLINED, H_FAILED };

static const wchar_t* kMainClass = L"SnapCapMain";
static const wchar_t* kOverlayClass = L"SnapCapOverlay";

// ---------------------------------------------------------------- state
static HINSTANCE g_inst;
static HWND g_main, g_overlay;
static HWND g_target;
static DWORD g_targetPid;
static std::wstring g_name;
static HWINEVENTHOOK g_hook;
static bool g_showOnScreen = true, g_askAdmin = true, g_showInVideo = true;
static bool g_recDot; // the overlay is showing the red recording dot
static UINT g_taskbarCreated, g_showRunning;
// Settings from snapcap.ini that are read once, at start.
struct Hotkey { UINT mods = 0, vk = 0; std::wstring text; };
static Hotkey g_hk[4];      // attach, show on screen, record, show in video (same order as the HK_ ids)
static int g_corner = 0;    // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
static rec::Options g_recOpt;
static bool g_follow = true;   // attach to whatever window is in focus
static bool g_forceBorderless = false; // make a game window borderless once it is out of exclusive fullscreen
static HWND g_exclusiveSeen, g_exclusiveTold, g_altEnterTried; // window last seen in exclusive fullscreen; the one the user was told about; the one we pressed Alt+Enter for
static ULONGLONG g_altEnterTick, g_borderlessUntil;
static HWND g_borderlessFixed; // the window we made borderless, kept at monitor size until g_borderlessUntil
static std::wstring runningText() {
    if (g_follow) return L"SnapCap is running in the tray and follows the window in focus. " + g_hk[2].text + L" starts and stops recording.";
    return L"SnapCap is running in the tray. Bring a window to the front and press " + g_hk[0].text + L" to attach, or use the tray icon.";
}
static HICON g_icon;
static std::vector<HWND> g_menuWindows;

// overlay drawing
static std::wstring g_textName, g_textFps;
static HFONT g_font;
static UINT g_fontDpi;
static SIZE g_sizeName, g_sizeFps;
static RECT g_lastRect;
static bool g_overlayShown;
static int g_tickCount;

// helper
static std::atomic<int> g_helper{H_OFF};
static HANDLE g_quitEvent, g_helperThread;
static SRWLOCK g_fpsLock = SRWLOCK_INIT;
static std::unordered_map<DWORD, float> g_helperFps;

// test/debug switches (not user features)
static HWINEVENTHOOK g_focusHook;
static std::wstring g_autoTitle, g_logPath, g_followTitle;
static bool g_noAdmin, g_autoRecorded;
static double g_exitAfter, g_recordAfter, g_recordSeconds;
static ULONGLONG g_attachTick, g_recordTick;
static ULONGLONG g_startTick;

static LONGLONG qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static LONGLONG qpf() { static LONGLONG f = [] { LARGE_INTEGER c; QueryPerformanceFrequency(&c); return c.QuadPart; }(); return f; }

static void trace(const char* what) { // test switch only: writes a marker to the --log file
    if (g_logPath.empty()) return;
    if (FILE* fp = _wfopen(g_logPath.c_str(), L"ab")) { fprintf(fp, "{\"trace\":\"%s\"}\n", what); fclose(fp); }
}

// ---------------------------------------------------------------- settings
static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\') + 1);
}
static std::wstring iniPath() { return exeDir() + L"snapcap.ini"; }
static std::wstring iniString(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"snapcap", key, def, buf, 512, iniPath().c_str());
    return buf;
}

// "Ctrl+Shift+F9" and the like. An empty value or "None" turns the hotkey off.
static Hotkey parseHotkey(const std::wstring& text, const wchar_t* def) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::wstring t = attempt == 0 ? text : std::wstring(def);
        Hotkey h;
        h.text = t;
        if (t.empty() || !_wcsicmp(t.c_str(), L"None")) { h.text = L"(no hotkey)"; return h; }
        bool bad = false;
        size_t pos = 0;
        while (pos <= t.size()) {
            size_t plus = t.find(L'+', pos);
            std::wstring part = t.substr(pos, plus == std::wstring::npos ? std::wstring::npos : plus - pos);
            while (!part.empty() && part.front() == L' ') part.erase(0, 1);
            while (!part.empty() && part.back() == L' ') part.pop_back();
            const wchar_t* p = part.c_str();
            if (!_wcsicmp(p, L"Ctrl") || !_wcsicmp(p, L"Control")) h.mods |= MOD_CONTROL;
            else if (!_wcsicmp(p, L"Shift")) h.mods |= MOD_SHIFT;
            else if (!_wcsicmp(p, L"Alt")) h.mods |= MOD_ALT;
            else if (!_wcsicmp(p, L"Win")) h.mods |= MOD_WIN;
            else if ((p[0] == L'F' || p[0] == L'f') && _wtoi(p + 1) >= 1 && _wtoi(p + 1) <= 24) h.vk = VK_F1 + _wtoi(p + 1) - 1;
            else if (part.size() == 1 && iswalnum(p[0])) h.vk = towupper(p[0]);
            else if (!_wcsicmp(p, L"Insert")) h.vk = VK_INSERT;
            else if (!_wcsicmp(p, L"Home")) h.vk = VK_HOME;
            else if (!_wcsicmp(p, L"End")) h.vk = VK_END;
            else if (!_wcsicmp(p, L"PageUp")) h.vk = VK_PRIOR;
            else if (!_wcsicmp(p, L"PageDown")) h.vk = VK_NEXT;
            else if (!_wcsicmp(p, L"Pause")) h.vk = VK_PAUSE;
            else if (!_wcsicmp(p, L"ScrollLock")) h.vk = VK_SCROLL;
            else if (!_wcsnicmp(p, L"0x", 2) && (wcstoul(p, nullptr, 16) & 0xFF)) h.vk = wcstoul(p, nullptr, 16) & 0xFF; // any other key, by number
            else bad = true;
            if (plus == std::wstring::npos) break;
            pos = plus + 1;
        }
        if (!bad && h.vk) return h;
    }
    return Hotkey{}; // not reached: the defaults always parse
}

static const wchar_t* kCornerNames[4] = {L"TopLeft", L"TopRight", L"BottomLeft", L"BottomRight"};
static const wchar_t* kHotkeyKeys[4] = {L"HotkeyAttach", L"HotkeyShowOnScreen", L"HotkeyRecord", L"HotkeyShowInVideo"};
static const wchar_t* kHotkeyDefaults[4] = {L"Ctrl+Shift+F9", L"Ctrl+Shift+F10", L"Ctrl+Shift+F11", L"Ctrl+Shift+F12"};

static void saveSettings() {
    const std::wstring ini = iniPath();
    auto put = [&](const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"snapcap", key, v.c_str(), ini.c_str()); };
    put(L"ShowOnScreen", g_showOnScreen ? L"1" : L"0");
    put(L"ShowInVideo", g_showInVideo ? L"1" : L"0");
    put(L"AskForExactFps", g_askAdmin ? L"1" : L"0");
    put(L"FollowFocus", g_follow ? L"1" : L"0");
    put(L"ForceBorderless", g_forceBorderless ? L"1" : L"0");
    for (int i = 0; i < 4; i++) put(kHotkeyKeys[i], g_hk[i].vk ? g_hk[i].text : L"None");
    put(L"Corner", kCornerNames[g_corner]);
    put(L"VideoFps", std::to_wstring(g_recOpt.fps));
    put(L"VideoHeight", std::to_wstring(g_recOpt.maxHeight));
    put(L"VideoBitrateMbps", std::to_wstring(g_recOpt.bitrateMbps));
    put(L"OutputFolder", g_recOpt.folder);
}

static void loadSettings() {
    const std::wstring ini = iniPath();
    const bool firstRun = GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES;
    g_showOnScreen = GetPrivateProfileIntW(L"snapcap", L"ShowOnScreen", 1, ini.c_str()) != 0;
    g_showInVideo = GetPrivateProfileIntW(L"snapcap", L"ShowInVideo", 1, ini.c_str()) != 0;
    g_askAdmin = GetPrivateProfileIntW(L"snapcap", L"AskForExactFps", 1, ini.c_str()) != 0;
    g_follow = GetPrivateProfileIntW(L"snapcap", L"FollowFocus", 1, ini.c_str()) != 0;
    g_forceBorderless = GetPrivateProfileIntW(L"snapcap", L"ForceBorderless", 0, ini.c_str()) != 0;
    for (int i = 0; i < 4; i++) g_hk[i] = parseHotkey(iniString(kHotkeyKeys[i], kHotkeyDefaults[i]), kHotkeyDefaults[i]);
    const std::wstring corner = iniString(L"Corner", L"TopLeft");
    g_corner = 0;
    for (int i = 0; i < 4; i++) if (!_wcsicmp(corner.c_str(), kCornerNames[i])) g_corner = i;
    g_recOpt.fps = GetPrivateProfileIntW(L"snapcap", L"VideoFps", 60, ini.c_str()) == 30 ? 30 : 60;
    g_recOpt.maxHeight = GetPrivateProfileIntW(L"snapcap", L"VideoHeight", 0, ini.c_str());
    g_recOpt.bitrateMbps = GetPrivateProfileIntW(L"snapcap", L"VideoBitrateMbps", 0, ini.c_str());
    g_recOpt.corner = g_corner;
    g_recOpt.folder = iniString(L"OutputFolder", L"");
    rec::setOptions(g_recOpt);
    saveSettings(); // writes every setting, so the file always lists all of them
    (void)firstRun;
}

static std::wstring recordingsFolder() {
    if (!g_recOpt.folder.empty()) return g_recOpt.folder;
    PWSTR videos = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos)) && videos) dir = std::wstring(videos) + L"\\SnapCap";
    CoTaskMemFree(videos);
    return dir;
}
// ---------------------------------------------------------------- name
static std::wstring versionString(const std::wstring& path, const wchar_t* key) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return L"";
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return L"";
    struct LangCp { WORD lang, cp; }* tr = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(LangCp)) return L"";
    wchar_t sub[96];
    swprintf(sub, 96, L"\\StringFileInfo\\%04x%04x\\%s", tr[0].lang, tr[0].cp, key);
    wchar_t* val = nullptr;
    if (!VerQueryValueW(data.data(), sub, (void**)&val, &len) || !val || len == 0) return L"";
    std::wstring s = val;
    while (!s.empty() && (s.back() == L' ' || s.back() == 0)) s.pop_back();
    return s;
}

// Product name the developer set, then the window title, then the file name.
static std::wstring resolveName(HWND w, DWORD pid) {
    std::wstring path, name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t p[1024];
        DWORD n = 1024;
        if (QueryFullProcessImageNameW(h, 0, p, &n)) path = p;
        CloseHandle(h);
    }
    if (!path.empty()) {
        name = versionString(path, L"ProductName");
        // Programs that ship with Windows all say "... Operating System" here.
        if (name.empty() || name.find(L"Operating System") != std::wstring::npos) {
            std::wstring d = versionString(path, L"FileDescription");
            if (!d.empty()) name = d;
            else if (!name.empty()) name.clear();
        }
    }
    if (name.empty()) {
        wchar_t t[256];
        if (GetWindowTextW(w, t, 256) > 0) name = t;
    }
    if (name.empty() && !path.empty()) {
        name = path.substr(path.find_last_of(L'\\') + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
    }
    if (name.empty()) name = L"Unknown";
    if (name.size() > 48) { name.resize(47); name += L"\x2026"; }
    return name;
}

// ---------------------------------------------------------------- capped fallback
// Counts frames of the attached window as they reach the screen, using Windows
// Graphics Capture on that one window. No picture is kept: each frame is
// released at once. Only runs while the helper is not supplying exact numbers.
struct Fallback {
    winrt::com_ptr<ID3D11Device> d3d;
    wgd::Direct3D11::IDirect3DDevice device{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    SRWLOCK lock = SRWLOCK_INIT;
    std::deque<LONGLONG> times;
    std::atomic<unsigned> generation{0};
    HWND window = nullptr;

    bool start(HWND target) {
        stop();
        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) return false;
            if (!d3d) {
                winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                       nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
                winrt::com_ptr<::IInspectable> insp;
                winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(d3d.as<IDXGIDevice>().get(), insp.put()));
                device = insp.as<wgd::Direct3D11::IDirect3DDevice>();
            }
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
            session = pool.CreateCaptureSession(item);
            using winrt::Windows::Foundation::Metadata::ApiInformation;
            try {
                if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
                    session.IsBorderRequired(false);
                if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
                    session.IsCursorCaptureEnabled(false);
            } catch (...) {}
            pool.FrameArrived([this, mine = generation.load()](wgc::Direct3D11CaptureFramePool const& p, auto&&) {
                if (auto frame = p.TryGetNextFrame()) frame.Close();
                else return;
                if (mine != generation.load()) return; // a session that is being shut down
                LONGLONG t = qpc();
                AcquireSRWLockExclusive(&lock);
                times.push_back(t);
                while (!times.empty() && times.front() < t - qpf()) times.pop_front();
                ReleaseSRWLockExclusive(&lock);
            });
            session.StartCapture();
            window = target;
            return true;
        } catch (...) {
            stop();
            return false;
        }
    }
    struct Closing {
        wgc::GraphicsCaptureSession session{nullptr};
        wgc::Direct3D11CaptureFramePool pool{nullptr};
        wgc::GraphicsCaptureItem item{nullptr};
    };
    static DWORD WINAPI closeThread(LPVOID p) {
        Closing* c = (Closing*)p;
        trace("close: session");
        try { if (c->session) c->session.Close(); } catch (...) {}
        trace("close: pool");
        try { if (c->pool) c->pool.Close(); } catch (...) {}
        delete c;
        trace("close: done");
        return 0;
    }
    // Shutting a capture session down talks to another Windows process, and in
    // testing that once stalled when the captured window was closing. It is done
    // on a throwaway thread so the tray and overlay can never freeze on it.
    void stop() {
        generation++;
        if (session || pool) {
            Closing* c = new Closing{session, pool, item};
            if (HANDLE th = CreateThread(nullptr, 0, closeThread, c, 0, nullptr)) CloseHandle(th);
        }
        session = nullptr; pool = nullptr; item = nullptr; window = nullptr;
        AcquireSRWLockExclusive(&lock);
        times.clear();
        ReleaseSRWLockExclusive(&lock);
    }
    bool running() const { return window != nullptr; }
    double fps() {
        LONGLONG now = qpc();
        AcquireSRWLockExclusive(&lock);
        while (!times.empty() && times.front() < now - qpf()) times.pop_front();
        double v = double(times.size());
        if (times.size() >= 2 && times.back() > times.front())
            v = double(times.size() - 1) * double(qpf()) / double(times.back() - times.front());
        ReleaseSRWLockExclusive(&lock);
        return v;
    }
};
static Fallback g_fallback;

// ---------------------------------------------------------------- helper
static void setHelper(int s) {
    g_helper = s;
    if (s != H_ON) {
        AcquireSRWLockExclusive(&g_fpsLock);
        g_helperFps.clear();
        ReleaseSRWLockExclusive(&g_fpsLock);
    }
    PostMessageW(g_main, WM_HELPER_CHANGED, 0, 0);
}

// Runs snapcap-fps.exe once and reads what it sends. With elevated=false it is started
// as the current user, which works when the account is in "Performance Log Users".
// With elevated=true it uses the standard administrator prompt. needElevation is set
// when a plain start produced nothing, so the caller can try the prompt.
static int runHelper(bool elevated, bool& needElevation) {
    needElevation = false;
    bool gotData = false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GUID id{};
    CoCreateGuid(&id);
    wchar_t pipeName[160];
    swprintf(pipeName, 160, L"%s%lu-%08lx%04x%04x", fpsproto::kPipePrefix, GetCurrentProcessId(), id.Data1, id.Data2, id.Data3);
    HANDLE pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 0, 65536, 0, nullptr);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE proc = nullptr;
    int result = H_FAILED;
    if (pipe != INVALID_HANDLE_VALUE) {
        std::wstring exe = exeDir() + L"snapcap-fps.exe";
        wchar_t params[256];
        swprintf(params, 256, L"--pipe \"%s\" --parent %lu", pipeName, GetCurrentProcessId());
        bool launched = false;
        if (elevated) {
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"runas";
            sei.lpFile = exe.c_str();
            sei.lpParameters = params;
            sei.nShow = SW_HIDE;
            launched = ShellExecuteExW(&sei) != FALSE;
            if (launched) proc = sei.hProcess;
        } else {
            std::wstring cmdline = L"\"" + exe + L"\" " + params;
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            launched = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
            if (launched) { proc = pi.hProcess; CloseHandle(pi.hThread); }
        }
        if (!launched) {
            result = elevated && GetLastError() == ERROR_CANCELLED ? H_DECLINED : H_FAILED;
        } else {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            bool connected = false;
            if (ConnectNamedPipe(pipe, &ov)) connected = true;
            else if (GetLastError() == ERROR_PIPE_CONNECTED) connected = true;
            else if (GetLastError() == ERROR_IO_PENDING) {
                HANDLE hs[3] = {ev, g_quitEvent, proc};
                DWORD w = WaitForMultipleObjects(proc ? 3 : 2, hs, FALSE, 15000);
                DWORD n = 0;
                connected = w == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &ov, &n, FALSE);
                if (!connected) CancelIo(pipe);
            }
            // Only accept data from the process we just started.
            if (connected && proc) {
                ULONG client = 0;
                DWORD started = GetProcessId(proc);
                if (started && GetNamedPipeClientProcessId(pipe, &client) && client != started) connected = false;
            }
            if (connected) {
                std::vector<unsigned char> buf(sizeof(fpsproto::Header) + fpsproto::kMaxEntries * sizeof(fpsproto::Entry));
                for (;;) {
                    ResetEvent(ev);
                    DWORD n = 0;
                    BOOL ok = ReadFile(pipe, buf.data(), DWORD(buf.size()), &n, &ov);
                    if (!ok && GetLastError() == ERROR_IO_PENDING) {
                        HANDLE hs[2] = {ev, g_quitEvent};
                        if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIo(pipe); break; }
                        ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
                    }
                    if (!ok) break; // helper has gone
                    if (n < sizeof(fpsproto::Header)) continue;
                    fpsproto::Header h;
                    memcpy(&h, buf.data(), sizeof(h));
                    if (h.magic != fpsproto::kMagic || h.count > fpsproto::kMaxEntries ||
                        n != sizeof(h) + h.count * sizeof(fpsproto::Entry)) continue;
                    if (!gotData) { gotData = true; setHelper(H_ON); } // the trace is running
                    AcquireSRWLockExclusive(&g_fpsLock);
                    g_helperFps.clear();
                    for (uint32_t i = 0; i < h.count; i++) {
                        fpsproto::Entry e;
                        memcpy(&e, buf.data() + sizeof(h) + i * sizeof(e), sizeof(e));
                        g_helperFps[e.pid] = float(e.fpsMilli) / 1000.0f;
                    }
                    ReleaseSRWLockExclusive(&g_fpsLock);
                }
                result = H_FAILED; // it was running and stopped
            }
        }
        CloseHandle(pipe); // the helper exits when its end breaks
    }
    if (proc) CloseHandle(proc);
    CloseHandle(ev);
    CoUninitialize();
    needElevation = !elevated && !gotData;
    return result;
}

// Tries a plain start first, and shows the administrator prompt only if Windows
// refused it. Runs on its own thread so the tray stays usable while the prompt shows.
static DWORD WINAPI helperThread(LPVOID) {
    bool need = false;
    int result = runHelper(false, need);
    if (need && WaitForSingleObject(g_quitEvent, 0) != WAIT_OBJECT_0) result = runHelper(true, need);
    if (WaitForSingleObject(g_quitEvent, 0) != WAIT_OBJECT_0) setHelper(result);
    return 0;
}

static void startHelper() {
    int s = g_helper.load();
    if (s == H_STARTING || s == H_ON) return;
    if (g_helperThread) { WaitForSingleObject(g_helperThread, 2000); CloseHandle(g_helperThread); }
    g_helper = H_STARTING;
    g_helperThread = CreateThread(nullptr, 0, helperThread, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------- tray
static void trayAdd() {
    NOTIFYICONDATAW n{sizeof(n)};
    n.hWnd = g_main;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_icon;
    wcscpy_s(n.szTip, L"SnapCap");
    Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}
static void trayTip(const std::wstring& tip) {
    NOTIFYICONDATAW n{sizeof(n)};
    n.hWnd = g_main;
    n.uID = 1;
    n.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
static void trayBalloon(const wchar_t* text) {
    NOTIFYICONDATAW n{sizeof(n)};
    n.hWnd = g_main;
    n.uID = 1;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcscpy_s(n.szInfoTitle, L"SnapCap");
    wcsncpy_s(n.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// A dark disc with a red dot, drawn at run time so no icon file is needed.
static HICON makeIcon() {
    const int S = 32;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    DWORD* px = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    if (!color) return LoadIconW(nullptr, IDI_APPLICATION);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            double dx = x + 0.5 - S / 2.0, dy = y + 0.5 - S / 2.0, d = sqrt(dx * dx + dy * dy);
            DWORD c = 0;
            if (d < 7.0) c = 0xFFE03030;
            else if (d < 15.0) c = 0xFF202124;
            px[y * S + x] = c;
        }
    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// ---------------------------------------------------------------- target
static bool isAttachable(HWND w) {
    if (!w || !IsWindow(w) || !IsWindowVisible(w)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (!wcscmp(cls, shell)) return false;
    return true;
}

static void positionOverlay(bool force);
static void refreshText();

// Decide where FPS comes from: the helper if it is running, otherwise the
// capped count from the attached window.
static void updateSources() {
    int h = g_helper.load();
    // While recording, the recorder's own capture supplies the capped count.
    bool want = g_target && h != H_ON && h != H_STARTING && rec::state() == rec::Idle;
    if (want && g_fallback.window != g_target) g_fallback.start(g_target);
    else if (!want && g_fallback.running()) g_fallback.stop();
}

static void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (hwnd == g_target && idObject == OBJID_WINDOW) positionOverlay(false);
}

static void detach() {
    if (rec::state() != rec::Idle) rec::stop(); // a recording ends with its window
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
    g_target = nullptr;
    g_targetPid = 0;
    g_name.clear();
    updateSources();
    positionOverlay(true);
    trayTip(L"SnapCap");
}

static void attach(HWND w, bool quiet = false) {
    w = GetAncestor(w, GA_ROOT);
    if (!isAttachable(w)) { if (!quiet) trayBalloon(L"SnapCap can't attach to that window."); return; }
    detach();
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(w, &pid);
    g_target = w;
    g_targetPid = pid;
    g_name = resolveName(w, pid);
    g_attachTick = GetTickCount64();
    g_autoRecorded = false;
    // Tells us when the window moves or resizes; runs in this process, nothing is loaded into the target.
    g_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, onWinEvent, pid, tid,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    updateSources();
    refreshText();
    positionOverlay(true);
    trayTip(L"SnapCap: " + g_name);
}

// ---------------------------------------------------------------- overlay
static UINT targetDpi() {
    UINT x = 96, y = 96;
    if (g_target) GetDpiForMonitor(MonitorFromWindow(g_target, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y);
    return x ? x : 96;
}

static void measure() {
    UINT dpi = targetDpi();
    if (!g_font || dpi != g_fontDpi) {
        if (g_font) DeleteObject(g_font);
        g_font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_fontDpi = dpi;
    }
    HDC dc = GetDC(g_overlay);
    HGDIOBJ old = SelectObject(dc, g_font);
    GetTextExtentPoint32W(dc, g_textName.c_str(), int(g_textName.size()), &g_sizeName);
    GetTextExtentPoint32W(dc, g_textFps.c_str(), int(g_textFps.size()), &g_sizeFps);
    SelectObject(dc, old);
    ReleaseDC(g_overlay, dc);
}

struct FpsReading { bool have; bool exact; double fps; };
static FpsReading readFps() {
    if (!g_target) return {false, false, 0};
    if (g_helper.load() == H_ON) {
        double v = 0;
        AcquireSRWLockShared(&g_fpsLock);
        auto it = g_helperFps.find(g_targetPid);
        if (it != g_helperFps.end()) v = it->second;
        ReleaseSRWLockShared(&g_fpsLock);
        return {true, true, v};
    }
    if (rec::state() == rec::Recording) return {true, false, rec::arrivalFps()};
    if (g_fallback.running()) return {true, false, g_fallback.fps()};
    return {false, false, 0};
}

static void logLine(const FpsReading& r) {
    if (g_logPath.empty()) return;
    FILE* fp = _wfopen(g_logPath.c_str(), L"ab");
    if (!fp) return;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    double unix = double((uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime)) / 1e7 - 11644473600.0;
    RECT o{};
    GetWindowRect(g_overlay, &o);
    wchar_t title[128] = L"";
    if (g_target) GetWindowTextW(g_target, title, 128);
    fprintf(fp, "{\"t\":%.3f,\"attached\":%d,\"title\":\"%ls\",\"name\":\"%ls\",\"source\":\"%s\",\"fps\":%.2f,\"helper\":%d,"
                "\"overlay_visible\":%d,\"ox\":%ld,\"oy\":%ld,\"ow\":%ld,\"oh\":%ld,\"text\":\"%ls%ls\"}\n",
            unix, g_target ? 1 : 0, title, g_name.c_str(), !r.have ? "none" : r.exact ? "exact" : "capped", r.fps, g_helper.load(),
            IsWindowVisible(g_overlay) ? 1 : 0, o.left, o.top, o.right - o.left, o.bottom - o.top, g_textName.c_str(), g_textFps.c_str());
    fclose(fp);
}

static void refreshText() {
    FpsReading r = readFps();
    wchar_t fps[64];
    if (!r.have) swprintf(fps, 64, L"-- fps");
    else if (r.exact) swprintf(fps, 64, L"%d fps", int(r.fps + 0.5));
    else swprintf(fps, 64, L"%d fps (capped)", int(r.fps + 0.5));
    std::wstring name = g_name + L"   |   ";
    rec::setOverlayText(name, fps);
    const bool dot = rec::state() == rec::Recording;
    if (name != g_textName || g_textFps != fps || targetDpi() != g_fontDpi || dot != g_recDot) {
        g_textName = name;
        g_textFps = fps;
        g_recDot = dot;
        measure();
        InvalidateRect(g_overlay, nullptr, FALSE);
    }
    logLine(r);
}

// Puts the overlay in the top-left of the target's client area, and hides it
// when the target is minimised, hidden, or covered at that spot by another window.
static void positionOverlay(bool force) {
    bool show = g_showOnScreen && g_target && IsWindow(g_target) && IsWindowVisible(g_target) && !IsIconic(g_target);
    RECT r{};
    if (show) {
        UINT dpi = targetDpi();
        int pad = MulDiv(9, dpi, 96), margin = MulDiv(12, dpi, 96);
        int w = g_sizeName.cx + g_sizeFps.cx + 2 * pad + (g_recDot ? MulDiv(20, dpi, 96) : 0);
        int h = std::max(g_sizeName.cy, g_sizeFps.cy) + MulDiv(8, dpi, 96);
        POINT p{0, 0};
        RECT cr{};
        ClientToScreen(g_target, &p);
        GetClientRect(g_target, &cr);
        const int ox = (g_corner & 1) ? p.x + cr.right - margin - w : p.x + margin;  // right-hand corners
        const int oy = (g_corner & 2) ? p.y + cr.bottom - margin - h : p.y + margin; // bottom corners
        r = {ox, oy, ox + w, oy + h};
        if (cr.right < w + 2 * margin || cr.bottom < h + 2 * margin) show = false;
        if (show) {
            // The overlay itself is skipped by this lookup because it is click-through.
            POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            HWND at = WindowFromPoint(c);
            if (!at || GetAncestor(at, GA_ROOT) != g_target) show = false;
        }
    }
    if (!show) {
        if (g_overlayShown || force) { ShowWindow(g_overlay, SW_HIDE); g_overlayShown = false; }
        return;
    }
    if (force || !g_overlayShown || !EqualRect(&r, &g_lastRect)) {
        SetWindowPos(g_overlay, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_lastRect = r;
        g_overlayShown = true;
        InvalidateRect(g_overlay, nullptr, FALSE);
    }
}

static LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HBRUSH b = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(mem, &r, b);
        DeleteObject(b);
        SetBkMode(mem, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(mem, g_font);
        int pad = MulDiv(9, g_fontDpi ? g_fontDpi : 96, 96);
        RECT a{pad, 0, r.right, r.bottom};
        if (g_recDot) { // red dot while recording; on screen only, never in the video
            int d = MulDiv(11, g_fontDpi ? g_fontDpi : 96, 96), top = (r.bottom - d) / 2;
            HBRUSH red = CreateSolidBrush(RGB(232, 48, 48));
            HGDIOBJ ob = SelectObject(mem, red), op = SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, pad, top, pad + d + 1, top + d + 1);
            SelectObject(mem, op);
            SelectObject(mem, ob);
            DeleteObject(red);
            a.left += MulDiv(20, g_fontDpi ? g_fontDpi : 96, 96);
        }
        SetTextColor(mem, RGB(232, 232, 232));
        DrawTextW(mem, g_textName.c_str(), -1, &a, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        a.left += g_sizeName.cx;
        SetTextColor(mem, RGB(120, 255, 120));
        DrawTextW(mem, g_textFps.c_str(), -1, &a, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- follow focus
// With "Follow the window in focus" on, SnapCap attaches to each ordinary window
// as it comes to the front. Things that take focus briefly (Start menu, Alt+Tab,
// the taskbar, pop-up tool windows) are ignored, so the overlay stays where it was.
// A recording keeps its window until it stops.
static bool isFollowable(HWND w) {
    if (!isAttachable(w) || GetWindow(w, GW_OWNER)) return false;
    if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    wchar_t title[256] = L"", cls[96] = L"";
    if (GetWindowTextW(w, title, 256) == 0) return false;
    GetClassNameW(w, cls, 96);
    for (const wchar_t* skip : {L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame",
                                L"ForegroundStaging", L"TaskListThumbnailWnd", L"NotifyIconOverflowWindow",
                                L"TopLevelWindowForOverflowXamlIsland", L"#32770"})
        if (!wcscmp(cls, skip)) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    if (!g_followTitle.empty() && wcsncmp(title, g_followTitle.c_str(), g_followTitle.size())) return false; // test switch only
    return true;
}

static void followFocus(HWND fg) {
    if (!g_follow || !fg || rec::state() != rec::Idle) return;
    fg = GetAncestor(fg, GA_ROOT);
    if (fg == g_target || !isFollowable(fg)) return;
    attach(fg, true);
}

static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) { followFocus(hwnd); }

static void setFollow(bool on) {
    g_follow = on;
    saveSettings();
    if (on) followFocus(GetForegroundWindow());
}

// ---------------------------------------------------------------- force borderless
// A topmost overlay can't draw over true exclusive fullscreen, and a window can't be
// moved out of that mode from outside. So: press Alt+Enter once for the user, and
// when the game is then a normal window, strip its title bar and borders and stretch
// it over its monitor. Only Unity windows and windows seen in exclusive fullscreen
// are touched, never ordinary programs.
static void forceBorderless() {
    if (!g_forceBorderless || !g_target || !IsWindow(g_target) || IsIconic(g_target)) return;
    QUERY_USER_NOTIFICATION_STATE qs;
    if (SUCCEEDED(SHQueryUserNotificationState(&qs)) && qs == QUNS_RUNNING_D3D_FULL_SCREEN &&
        GetAncestor(GetForegroundWindow(), GA_ROOT) == g_target) {
        g_exclusiveSeen = g_target;
        if (g_altEnterTried != g_target) { // press Alt+Enter once, which makes most games a window
            g_altEnterTried = g_target;
            g_altEnterTick = GetTickCount64();
            INPUT in[4]{};
            const WORD keys[4] = {VK_MENU, VK_RETURN, VK_RETURN, VK_MENU};
            for (int i = 0; i < 4; i++) {
                in[i].type = INPUT_KEYBOARD;
                in[i].ki.wVk = keys[i];
                in[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0;
            }
            SendInput(4, in, sizeof(INPUT));
        } else if (g_exclusiveTold != g_target && GetTickCount64() - g_altEnterTick > 3000) {
            g_exclusiveTold = g_target; // Alt+Enter did nothing: this game doesn't allow it
            trayBalloon(L"This game is in exclusive fullscreen, so the overlay can't show on it, and it ignored Alt+Enter. Switch it to borderless or windowed in its own settings.");
        }
        return;
    }
    g_altEnterTried = nullptr; // out of exclusive fullscreen: ready for the next time
    g_exclusiveTold = nullptr;
    wchar_t cls[64] = L"";
    GetClassNameW(g_target, cls, 64);
    if (g_target != g_exclusiveSeen && wcscmp(cls, L"UnityWndClass")) return;
    LONG_PTR style = GetWindowLongPtrW(g_target, GWL_STYLE);
    const bool hasCaption = (style & WS_CAPTION) == WS_CAPTION;
    // For a few seconds after the change the game may resize its own window back
    // (Unity does, to its windowed size), which pushes the bottom off the screen.
    // So keep checking the size until it holds.
    const bool settling = g_target == g_borderlessFixed && GetTickCount64() < g_borderlessUntil;
    if (!hasCaption && !settling) return; // already borderless (or never had a title bar)
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(g_target, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT& m = mi.rcMonitor;
    RECT cur{};
    GetWindowRect(g_target, &cur);
    if (!hasCaption && EqualRect(&cur, &m)) return;
    if (hasCaption) {
        SetWindowLongPtrW(g_target, GWL_STYLE, (style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        g_borderlessFixed = g_target;
        g_borderlessUntil = GetTickCount64() + 6000;
    }
    SetWindowPos(g_target, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// ---------------------------------------------------------------- recording
static void toggleRecord() {
    rec::State s = rec::state();
    if (s == rec::Recording || s == rec::Starting) { rec::stop(); return; }
    if (s != rec::Idle) return; // still finishing the last file
    if (!g_target) { trayBalloon((L"Attach to a window first (" + g_hk[0].text + L"), then start recording.").c_str()); return; }
    rec::setShowInVideo(g_showInVideo);
    rec::setOverlayText(g_textName, g_textFps);
    if (!rec::start(g_target, g_targetPid, g_name, g_main, WM_RECORD_DONE)) return;
    g_recordTick = GetTickCount64();
    // Keeps the on-screen overlay out of the picture if whole-screen capture has to be used.
    SetWindowDisplayAffinity(g_overlay, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    updateSources();
    trayTip(L"SnapCap: recording " + g_name);
}

static void recordDone() {
    rec::Result r = rec::result();
    SetWindowDisplayAffinity(g_overlay, 0);
    if (r.ok) {
        std::wstring msg = L"Saved " + r.file.substr(r.file.find_last_of(L'\\') + 1) + L" in Videos\\SnapCap.";
        if (!r.audio) msg += L" No sound: the application's audio could not be opened.";
        trayBalloon(msg.c_str());
    } else {
        trayBalloon((L"Recording failed: " + r.error + L".").c_str());
    }
    if (!g_logPath.empty()) { // test switch only
        if (FILE* fp = _wfopen(g_logPath.c_str(), L"ab")) {
            std::wstring f = r.file;
            for (auto& c : f) if (c == L'\\') c = L'/';
            fprintf(fp, "{\"record\":1,\"ok\":%d,\"file\":\"%ls\",\"error\":\"%ls\",\"audio\":%d,\"screen_method\":%d,\"hw_encoder\":%d,"
                        "\"frames\":%lld,\"dropped\":%lld,\"seconds\":%.3f,\"w\":%d,\"h\":%d,\"start\":%.3f,\"probe_window\":%d,\"probe_screen\":%d,\"late\":%lld,\"switches\":%d}\n",
                    r.ok ? 1 : 0, f.c_str(), r.error.c_str(), r.audio ? 1 : 0, r.screenMethod ? 1 : 0, r.hwEncoder ? 1 : 0,
                    r.frames, r.dropped, r.seconds, r.width, r.height, r.startUnix, r.probeWindow, r.probeScreen, r.late, r.switches);
            fclose(fp);
        }
    }
    updateSources();
    refreshText();
    positionOverlay(true);
    trayTip(g_target ? L"SnapCap: " + g_name : std::wstring(L"SnapCap"));
    followFocus(GetForegroundWindow()); // the recording held its window; catch up with the focus now
}

#include "settings_window.inc"

// ---------------------------------------------------------------- menu
static BOOL CALLBACK enumProc(HWND w, LPARAM) {
    if (!isAttachable(w) || GetWindow(w, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (GetWindowTextLengthW(w) == 0) return TRUE;
    BOOL cloaked = FALSE; // windows on other virtual desktops, suspended store apps
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    if (g_menuWindows.size() < CMD_WINDOW_LAST - CMD_WINDOW_FIRST) g_menuWindows.push_back(w);
    return TRUE;
}

static void showMenu() {
    HMENU menu = CreatePopupMenu(), windows = CreatePopupMenu();
    g_menuWindows.clear();
    EnumWindows(enumProc, 0);
    for (size_t i = 0; i < g_menuWindows.size(); i++) {
        wchar_t t[80];
        GetWindowTextW(g_menuWindows[i], t, 60);
        std::wstring s;
        for (wchar_t* c = t; *c; c++) { s += *c; if (*c == L'&') s += L'&'; }
        AppendMenuW(windows, MF_STRING | (g_menuWindows[i] == g_target ? MF_CHECKED : 0), CMD_WINDOW_FIRST + UINT(i), s.c_str());
    }
    if (g_menuWindows.empty()) AppendMenuW(windows, MF_STRING | MF_GRAYED, 0, L"(no windows)");

    std::wstring head = g_target ? L"Attached: " + g_name : L"Not attached";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, head.c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)windows, L"Attach to window");
    AppendMenuW(menu, MF_STRING | (g_follow ? MF_CHECKED : 0), CMD_FOLLOW, L"Follow the window in focus");
    AppendMenuW(menu, MF_STRING | (g_target ? 0 : MF_GRAYED), CMD_DETACH, L"Detach");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    rec::State rs = rec::state();
    AppendMenuW(menu, MF_STRING | ((rs == rec::Idle && !g_target) || rs == rec::Stopping ? MF_GRAYED : 0), CMD_RECORD,
                (rs == rec::Idle ? L"Start recording\t" + g_hk[2].text : rs == rec::Stopping ? std::wstring(L"Finishing the recording...") : L"Stop recording\t" + g_hk[2].text).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_showOnScreen ? MF_CHECKED : 0), CMD_SHOW, (L"Show on screen\t" + g_hk[1].text).c_str());
    AppendMenuW(menu, MF_STRING | (g_showInVideo ? MF_CHECKED : 0), CMD_VIDEO, (L"Show in video\t" + g_hk[3].text).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    int h = g_helper.load();
    const wchar_t* status = h == H_ON ? L"FPS: exact" : h == H_STARTING ? L"FPS: waiting for the administrator prompt"
                          : h == H_DECLINED ? L"FPS: capped at the screen's refresh rate (prompt declined)"
                          : h == H_FAILED ? L"FPS: capped at the screen's refresh rate (helper not running)"
                          : L"FPS: capped at the screen's refresh rate";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
    if (h != H_ON && h != H_STARTING) AppendMenuW(menu, MF_STRING, CMD_START_HELPER, L"Turn on exact FPS");
    AppendMenuW(menu, MF_STRING | (g_askAdmin ? MF_CHECKED : 0), CMD_ASK_ADMIN, L"Turn on exact FPS when SnapCap starts");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_FOLDER, L"Open recordings folder");
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"Attach to the window in front\t" + g_hk[0].text).c_str());
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_main);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= CMD_WINDOW_FIRST && cmd <= CMD_WINDOW_LAST) {
        HWND w = g_menuWindows[cmd - CMD_WINDOW_FIRST];
        g_follow = false; // a window picked by hand stays picked
        saveSettings();
        if (w == g_target) detach(); else attach(w);
    } else if (cmd == CMD_DETACH) { g_follow = false; saveSettings(); detach(); }
    else if (cmd == CMD_FOLLOW) setFollow(!g_follow);
    else if (cmd == CMD_SHOW) { g_showOnScreen = !g_showOnScreen; saveSettings(); positionOverlay(true); }
    else if (cmd == CMD_VIDEO) { g_showInVideo = !g_showInVideo; saveSettings(); rec::setShowInVideo(g_showInVideo); }
    else if (cmd == CMD_RECORD) toggleRecord();
    else if (cmd == CMD_SETTINGS) openSettings();
    else if (cmd == CMD_FOLDER) { std::wstring d = recordingsFolder(); SHCreateDirectoryExW(nullptr, d.c_str(), nullptr); ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    else if (cmd == CMD_ASK_ADMIN) { g_askAdmin = !g_askAdmin; saveSettings(); }
    else if (cmd == CMD_START_HELPER) startHelper();
    else if (cmd == CMD_EXIT) DestroyWindow(g_main);
}

// ---------------------------------------------------------------- main window
struct FindArg { const std::wstring* prefix; HWND found; };
static BOOL CALLBACK findByTitle(HWND w, LPARAM l) {
    FindArg* a = (FindArg*)l;
    wchar_t t[256];
    if (IsWindowVisible(w) && GetWindowTextW(w, t, 256) > 0 && !wcsncmp(t, a->prefix->c_str(), a->prefix->size())) {
        a->found = w;
        return FALSE;
    }
    return TRUE;
}

static void tick() {
    g_tickCount++;
    if (g_target && !IsWindow(g_target)) detach();
    if (!g_target && !g_autoTitle.empty()) { // test switch only
        FindArg a{&g_autoTitle, nullptr};
        EnumWindows(findByTitle, (LPARAM)&a);
        if (a.found) attach(a.found);
    }
    if (g_recordAfter > 0 && g_target) { // test switches only: record by itself
        rec::State s = rec::state();
        if (!g_autoRecorded && s == rec::Idle && double(GetTickCount64() - g_attachTick) / 1000.0 >= g_recordAfter) {
            g_autoRecorded = true;
            toggleRecord();
        } else if (g_autoRecorded && s == rec::Recording && g_recordSeconds > 0 &&
                   double(GetTickCount64() - g_recordTick) / 1000.0 >= g_recordSeconds) {
            rec::stop();
        }
    }
    if (g_tickCount % 5 == 0) followFocus(GetForegroundWindow()); // in case a focus change was missed
    if (g_tickCount % 5 == 0) forceBorderless();
    if (g_tickCount % 5 == 0) refreshText();              // twice a second
    positionOverlay(g_overlayShown && g_tickCount % 5 == 0); // also re-assert "on top" twice a second
    if (g_exitAfter > 0 && double(GetTickCount64() - g_startTick) / 1000.0 >= g_exitAfter) DestroyWindow(g_main);
}

static LRESULT CALLBACK mainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == TIMER_TICK) tick();
        return 0;
    case WM_HOTKEY:
        if (w == HK_ATTACH) {
            HWND fg = GetForegroundWindow();
            g_follow = false; // the hotkey picks one window and stays on it
            saveSettings();
            if (fg && GetAncestor(fg, GA_ROOT) == g_target) detach();
            else if (fg) attach(fg);
        } else if (w == HK_SHOW) {
            g_showOnScreen = !g_showOnScreen;
            saveSettings();
            positionOverlay(true);
        } else if (w == HK_RECORD) {
            toggleRecord();
        } else if (w == HK_VIDEO) {
            g_showInVideo = !g_showInVideo;
            saveSettings();
            rec::setShowInVideo(g_showInVideo);
        }
        return 0;
    case WM_RECORD_DONE:
        recordDone();
        return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_CONTEXTMENU || LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) showMenu();
        return 0;
    case WM_HELPER_CHANGED:
        updateSources();
        refreshText();
        if (g_helper.load() == H_DECLINED)
            trayBalloon(L"Exact FPS is off. FPS shown is capped at the screen's refresh rate. You can turn exact FPS on from the tray menu.");
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW n{sizeof(n)};
        n.hWnd = h;
        n.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &n);
        PostQuitMessage(0);
        return 0;
    }
    }
    if (m == g_taskbarCreated && g_taskbarCreated) { trayAdd(); return 0; } // Explorer restarted
    if (m == g_showRunning && g_showRunning) { trayBalloon(runningText().c_str()); return 0; } // started a second time
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int videoText = -1, screenText = -1; // test overrides of the two settings, not saved
    bool openAtStart = false;
    for (int i = 1; argv && i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--attach-title") g_autoTitle = next();
        else if (a == L"--log") g_logPath = next();
        else if (a == L"--no-admin") g_noAdmin = true;
        else if (a == L"--exit-after") g_exitAfter = _wtof(next().c_str());
        else if (a == L"--record-after") g_recordAfter = _wtof(next().c_str());
        else if (a == L"--record-seconds") g_recordSeconds = _wtof(next().c_str());
        else if (a == L"--video-text") videoText = _wtoi(next().c_str());
        else if (a == L"--show-on-screen") screenText = _wtoi(next().c_str());
        else if (a == L"--follow-title") g_followTitle = next();
        else if (a == L"--open-settings") openAtStart = true;
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\SnapCapSingleInstance");
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    g_showRunning = RegisterWindowMessageW(L"SnapCapShowRunning");
    if (alreadyRunning) { // already running: ask that copy to say so
        if (HWND other = FindWindowW(kMainClass, L"SnapCap")) PostMessageW(other, g_showRunning, 0, 0);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    loadSettings();
    if (videoText >= 0) g_showInVideo = videoText != 0;
    if (screenText >= 0) g_showOnScreen = screenText != 0;
    rec::setShowInVideo(g_showInVideo);
    for (auto& h : g_hk) h.text = hotkeyText(h.mods, h.vk); // one spelling everywhere
    g_startTick = GetTickCount64();
    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_icon = makeIcon();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.lpfnWndProc = mainProc;
    wc.lpszClassName = kMainClass;
    RegisterClassW(&wc);
    wc.lpfnWndProc = overlayProc;
    wc.lpszClassName = kOverlayClass;
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, kMainClass, L"SnapCap", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                kOverlayClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_overlay, 0, 220, LWA_ALPHA);

    trayAdd();
    std::wstring taken = registerHotkeys();

    if (!taken.empty())
        trayBalloon((L"SnapCap is running in the tray. Another program is already using " + taken + L", so use the tray menu for that, or choose another hotkey in Settings.").c_str());
    else if (g_autoTitle.empty()) trayBalloon(runningText().c_str());
    if (g_askAdmin && !g_noAdmin) startHelper();
    if (!g_autoTitle.empty()) g_follow = false; // tests that attach by title do not follow focus (not saved)
    g_focusHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0, 0,
                                  WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    followFocus(GetForegroundWindow());
    SetTimer(g_main, TIMER_TICK, 100, nullptr);
    if (openAtStart) openSettings(); // test switch only

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &msg)) continue; // Tab, Enter and Esc in the settings window
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    rec::shutdown(); // finish the MP4 if one is being written
    SetEvent(g_quitEvent);
    if (g_hook) UnhookWinEvent(g_hook);
    g_fallback.stop();
    if (g_helperThread) { WaitForSingleObject(g_helperThread, 3000); CloseHandle(g_helperThread); }
    if (g_font) DeleteObject(g_font);
    g_fallback.device = nullptr;
    g_fallback.d3d = nullptr;
    if (single) CloseHandle(single);
    return 0;
}
