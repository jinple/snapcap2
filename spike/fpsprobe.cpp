// Step 0 FPS helper probe. Must run as administrator.
// Opens a real-time Windows event trace on the graphics providers and counts
// events per second, per event id, for processes with a given exe name.
// It records every event id so the results show which one equals the true
// frame rate on each graphics API.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

static const wchar_t* kSession = L"SnapCapSpikeFps";

struct Provider { const char* name; GUID guid; };
static const Provider kProviders[] = {
    {"DxgKrnl", {0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}}},
    {"DXGI",    {0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}}},
    {"D3D9",    {0x783aca0a, 0x790e, 0x4d7f, {0x84, 0x51, 0xaa, 0x85, 0x05, 0x11, 0xc6, 0xb9}}},
};
static const int kProviderCount = sizeof(kProviders) / sizeof(kProviders[0]);

struct Props {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[128];
};

static std::wstring g_exe;
static std::unordered_map<DWORD, bool> g_pidMatch;
// (unix second, pid, provider index, event id) -> count
static std::map<std::tuple<int64_t, DWORD, int, int>, uint32_t> g_counts;
static uint64_t g_total = 0;

static bool pidMatches(DWORD pid) {
    auto it = g_pidMatch.find(pid);
    if (it != g_pidMatch.end()) return it->second;
    bool match = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            match = _wcsicmp(base, g_exe.c_str()) == 0;
        }
        CloseHandle(h);
    }
    g_pidMatch[pid] = match;
    return match;
}

static void WINAPI onEvent(PEVENT_RECORD rec) {
    g_total++;
    DWORD pid = rec->EventHeader.ProcessId;
    if (pid == 0 || pid == 0xFFFFFFFF || !pidMatches(pid)) return;
    int prov = -1;
    for (int i = 0; i < kProviderCount; i++)
        if (IsEqualGUID(rec->EventHeader.ProviderId, kProviders[i].guid)) { prov = i; break; }
    if (prov < 0) return;
    int64_t sec = int64_t(rec->EventHeader.TimeStamp.QuadPart / 10000000LL) - 11644473600LL;
    g_counts[{sec, pid, prov, int(rec->EventHeader.EventDescriptor.Id)}]++;
}

static void stopSession() {
    Props p{};
    p.p.Wnode.BufferSize = sizeof(p);
    p.p.LoggerNameOffset = offsetof(Props, name);
    ControlTraceW(0, kSession, &p.p, EVENT_TRACE_CONTROL_STOP);
}

struct StopArgs { std::wstring stopFile; double seconds; };
static DWORD WINAPI stopThread(LPVOID arg) {
    StopArgs* a = (StopArgs*)arg;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        Sleep(200);
        if (double(GetTickCount64() - start) / 1000.0 >= a->seconds) break;
        if (!a->stopFile.empty() && GetFileAttributesW(a->stopFile.c_str()) != INVALID_FILE_ATTRIBUTES) break;
    }
    Sleep(1500); // let the last buffers flush
    stopSession();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring out = L"etw.csv", readyFile;
    StopArgs sa{L"", 600};
    ULONGLONG keywords = 0; // 0 = every keyword
    g_exe = L"testwin.exe";
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--exe") g_exe = next();
        else if (a == L"--out") out = next();
        else if (a == L"--stopfile") sa.stopFile = next();
        else if (a == L"--readyfile") readyFile = next();
        else if (a == L"--seconds") sa.seconds = _wtof(next().c_str());
        else if (a == L"--keywords") keywords = _wcstoui64(next().c_str(), nullptr, 16);
    }

    stopSession(); // clear any stale session from an earlier run
    Props props{};
    props.p.Wnode.BufferSize = sizeof(props);
    props.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props.p.Wnode.ClientContext = 1;
    props.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props.p.LoggerNameOffset = offsetof(Props, name);
    props.p.BufferSize = 256;
    props.p.MinimumBuffers = 64;
    props.p.MaximumBuffers = 256;
    props.p.FlushTimer = 1;
    TRACEHANDLE session = 0;
    ULONG rc = StartTraceW(&session, kSession, &props.p);
    FILE* fp = _wfopen(out.c_str(), L"wb");
    if (!fp) return 3;
    if (rc != ERROR_SUCCESS) {
        fprintf(fp, "error,StartTrace,%lu\n", rc);
        fclose(fp);
        return 1;
    }
    for (int i = 0; i < kProviderCount; i++) {
        rc = EnableTraceEx2(session, &kProviders[i].guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords, 0, 0, nullptr);
        if (rc != ERROR_SUCCESS) fprintf(fp, "error,EnableTrace %s,%lu\n", kProviders[i].name, rc);
    }

    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = (LPWSTR)kSession;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = onEvent;
    TRACEHANDLE trace = OpenTraceW(&lf);
    if (trace == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(fp, "error,OpenTrace,%lu\n", GetLastError());
        fclose(fp);
        stopSession();
        return 1;
    }
    if (!readyFile.empty()) {
        FILE* r = _wfopen(readyFile.c_str(), L"wb");
        if (r) { fputs("ready\n", r); fclose(r); }
    }
    HANDLE th = CreateThread(nullptr, 0, stopThread, &sa, 0, nullptr);
    ProcessTrace(&trace, 1, nullptr, nullptr); // blocks until the session stops
    WaitForSingleObject(th, INFINITE);
    CloseTrace(trace);

    fprintf(fp, "sec,pid,provider,id,count\n");
    for (auto& kv : g_counts)
        fprintf(fp, "%lld,%lu,%s,%d,%u\n", (long long)std::get<0>(kv.first), std::get<1>(kv.first),
                kProviders[std::get<2>(kv.first)].name, std::get<3>(kv.first), kv.second);
    fprintf(fp, "total,%llu,,,\n", (unsigned long long)g_total);
    fclose(fp);
    return 0;
}
