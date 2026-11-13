// Step 0 overlay probe: a small click-through, always-on-top window placed over
// a target window, repainting text twice a second. Used to measure what such a
// window costs the target in fps.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>

static std::wstring g_text = L"snapcap-testwin  |  -- fps";

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &r, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(120, 255, 120));
        HFONT f = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        r.left += 8;
        DrawTextW(dc, g_text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        SelectObject(dc, old);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
    std::wstring title;
    double seconds = 10;
    bool exclude = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--title") title = next();
        else if (a == L"--seconds") seconds = _wtof(next().c_str());
        else if (a == L"--exclude") exclude = true;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND target = nullptr;
    for (int i = 0; i < 50 && !target; i++) {
        target = FindWindowW(L"SnapCapTestWin", title.c_str());
        if (!target) Sleep(100);
    }
    if (!target) { fprintf(stderr, "target window not found\n"); return 2; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SnapCapOverlayProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                wc.lpszClassName, L"", WS_POPUP, 0, 0, 300, 34, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, 0, 215, LWA_ALPHA);
    if (exclude && !SetWindowDisplayAffinity(hwnd, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */))
        fprintf(stderr, "SetWindowDisplayAffinity failed %lu\n", GetLastError());

    ULONGLONG start = GetTickCount64();
    int tick = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (!IsWindow(target) || double(GetTickCount64() - start) / 1000.0 >= seconds) break;
        POINT p{0, 0};
        ClientToScreen(target, &p);
        SetWindowPos(hwnd, HWND_TOPMOST, p.x + 16, p.y + 16, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        wchar_t buf[128];
        swprintf(buf, 128, L"snapcap-testwin  |  %d fps", 100 + (tick++ % 50));
        g_text = buf;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        Sleep(500);
    }
    DestroyWindow(hwnd);
    return 0;
}
