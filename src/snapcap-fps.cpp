// snapcap-fps.exe: the only part of SnapCap that runs as administrator.
//
// It does one thing. It listens to the Windows kernel graphics provider for the
// "present" event (id 184), which Windows logs once per frame a program shows,
// whatever graphics API the program uses. Four times a second it writes each
// presenting program's frame rate to a pipe that snapcap.exe opened.
//
// It has no window, reads no input from the pipe, opens no files, and exits
// when snapcap.exe exits or the pipe closes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define INITGUID
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <shellapi.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "fps_protocol.h"

static const wchar_t* kSession = L"SnapCapFps";
// Microsoft-Windows-DxgKrnl
static const GUID kDxgKrnl = {0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
static const USHORT kPresentEvent = 184;
static const ULONGLONG kPresentKeyword = 0x8000000;

struct Props {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[128];
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<DWORD, std::deque<LONGLONG>> g_times; // pid -> present times (QPC ticks), last second

static void WINAPI onEvent(PEVENT_RECORD rec) {
    if (rec->EventHeader.EventDescriptor.Id != kPresentEvent) return;
    if (!IsEqualGUID(rec->EventHeader.ProviderId, kDxgKrnl)) return;
    DWORD pid = rec->EventHeader.ProcessId;
    if (pid == 0 || pid == 0xFFFFFFFF) return;
    AcquireSRWLockExclusive(&g_lock);
    g_times[pid].push_back(rec->EventHeader.TimeStamp.QuadPart);
    ReleaseSRWLockExclusive(&g_lock);
}

static void controlSession(ULONG code) {
    Props p{};
    p.p.Wnode.BufferSize = sizeof(p);
    p.p.LoggerNameOffset = offsetof(Props, name);
    ControlTraceW(0, kSession, &p.p, code);
}

static DWORD WINAPI traceThread(LPVOID arg) {
    TRACEHANDLE trace = *(TRACEHANDLE*)arg;
    ProcessTrace(&trace, 1, nullptr, nullptr); // returns when the session stops
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring pipeName;
    DWORD parentPid = 0;
    for (int i = 1; argv && i + 1 < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--pipe") pipeName = argv[++i];
        else if (a == L"--parent") parentPid = wcstoul(argv[++i], nullptr, 10);
    }
    const size_t prefixLen = wcslen(fpsproto::kPipePrefix);
    if (parentPid == 0 || pipeName.compare(0, prefixLen, fpsproto::kPipePrefix) != 0) return 2;

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent) return 2;
    // SECURITY_IDENTIFICATION: whoever owns the pipe cannot act with this process's rights.
    HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid != parentPid) return 3;

    controlSession(EVENT_TRACE_CONTROL_STOP); // clear a session left behind by a killed helper
    Props props{};
    props.p.Wnode.BufferSize = sizeof(props);
    props.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props.p.Wnode.ClientContext = 1; // timestamps are QueryPerformanceCounter ticks
    props.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props.p.LoggerNameOffset = offsetof(Props, name);
    props.p.BufferSize = 64;
    props.p.MinimumBuffers = 8;
    props.p.MaximumBuffers = 32;
    props.p.FlushTimer = 1;
    TRACEHANDLE session = 0;
    if (StartTraceW(&session, kSession, &props.p) != ERROR_SUCCESS) return 1; // not elevated, most likely

    // Ask for the present keyword only, and within it only event 184.
    EVENT_FILTER_EVENT_ID ids{};
    ids.FilterIn = TRUE;
    ids.Count = 1;
    ids.Events[0] = kPresentEvent;
    EVENT_FILTER_DESCRIPTOR fd{};
    fd.Ptr = (ULONGLONG)(ULONG_PTR)&ids;
    fd.Size = sizeof(ids);
    fd.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS ep{};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableFilterDesc = &fd;
    ep.FilterDescCount = 1;
    ULONG rc = EnableTraceEx2(session, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                              kPresentKeyword, 0, 0, &ep);
    if (rc != ERROR_SUCCESS) // no id filter on this Windows: the callback filters instead
        rc = EnableTraceEx2(session, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                            kPresentKeyword, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) { controlSession(EVENT_TRACE_CONTROL_STOP); return 1; }

    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = (LPWSTR)kSession;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = onEvent;
    TRACEHANDLE trace = OpenTraceW(&lf);
    if (trace == INVALID_PROCESSTRACE_HANDLE) { controlSession(EVENT_TRACE_CONTROL_STOP); return 1; }
    HANDLE th = CreateThread(nullptr, 0, traceThread, &trace, 0, nullptr);

    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    std::vector<unsigned char> msg;
    for (;;) {
        if (WaitForSingleObject(parent, 250) != WAIT_TIMEOUT) break; // snapcap.exe has gone
        controlSession(EVENT_TRACE_CONTROL_FLUSH);                   // Windows otherwise delivers once a second
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONGLONG cutoff = now.QuadPart - qf.QuadPart; // one second ago

        msg.resize(sizeof(fpsproto::Header));
        uint32_t count = 0;
        AcquireSRWLockExclusive(&g_lock);
        for (auto it = g_times.begin(); it != g_times.end();) {
            auto& q = it->second;
            while (!q.empty() && q.front() < cutoff) q.pop_front();
            if (q.empty()) { it = g_times.erase(it); continue; }
            // Rate between the first and last present in the window: steady for a
            // steady program, with no off-by-one from where the window edges fall.
            double fps = double(q.size());
            if (q.size() >= 2 && q.back() > q.front())
                fps = double(q.size() - 1) * double(qf.QuadPart) / double(q.back() - q.front());
            if (count < fpsproto::kMaxEntries) {
                fpsproto::Entry e{it->first, uint32_t(fps * 1000.0 + 0.5)};
                msg.insert(msg.end(), (unsigned char*)&e, (unsigned char*)&e + sizeof(e));
                count++;
            }
            ++it;
        }
        ReleaseSRWLockExclusive(&g_lock);
        fpsproto::Header h{fpsproto::kMagic, count};
        memcpy(msg.data(), &h, sizeof(h));
        DWORD written = 0;
        if (!WriteFile(pipe, msg.data(), DWORD(msg.size()), &written, nullptr)) break; // pipe closed
    }

    controlSession(EVENT_TRACE_CONTROL_STOP);
    if (th) WaitForSingleObject(th, 3000);
    CloseTrace(trace);
    CloseHandle(pipe);
    CloseHandle(parent);
    return 0;
}
