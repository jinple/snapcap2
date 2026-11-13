// The settings window. Included into snapcap.cpp (it uses that file's state
// directly). Plain Windows controls built in code: no dialog resource, no extra
// program. Changes apply when Save is pressed; nothing needs a restart.

enum {
    ID_FOLLOW = 101, ID_SCREEN, ID_VIDEO, ID_ADMIN, ID_BORDERLESS,
    ID_CORNER = 110,
    ID_HK0 = 120, // ..123: attach, show on screen, record, show in video
    ID_FPS = 130, ID_SIZE, ID_RATE,
    ID_FOLDER = 140, ID_BROWSE, ID_STATUS
};

static HWND g_settings;
static HFONT g_setFont;
static WORD g_hkShown[4]; // what each hotkey box was filled with, to tell "unchanged" from "changed"

static const struct { const wchar_t* name; UINT vk; } kNamedKeys[] = {
    {L"Insert", VK_INSERT}, {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR},
    {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE}, {L"ScrollLock", VK_SCROLL}};

// The text form used in snapcap.ini and in menus, e.g. "Ctrl+Shift+F9".
static std::wstring hotkeyText(UINT mods, UINT vk) {
    if (!vk) return L"(no hotkey)";
    std::wstring t;
    if (mods & MOD_CONTROL) t += L"Ctrl+";
    if (mods & MOD_SHIFT) t += L"Shift+";
    if (mods & MOD_ALT) t += L"Alt+";
    if (mods & MOD_WIN) t += L"Win+";
    wchar_t key[16];
    if (vk >= VK_F1 && vk <= VK_F24) swprintf(key, 16, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) swprintf(key, 16, L"%c", wchar_t(vk));
    else {
        swprintf(key, 16, L"0x%02X", vk); // any other key is kept by its Windows key number
        for (auto& k : kNamedKeys) if (k.vk == vk) swprintf(key, 16, L"%s", k.name);
    }
    return t + key;
}

static std::wstring registerHotkeys() { // returns the ones another program already holds
    std::wstring taken;
    for (int i = 0; i < 4; i++) { // ids HK_ATTACH..HK_VIDEO are 1..4, in the same order as g_hk
        UnregisterHotKey(g_main, i + 1);
        if (g_hk[i].vk && !RegisterHotKey(g_main, i + 1, g_hk[i].mods | MOD_NOREPEAT, g_hk[i].vk))
            taken += (taken.empty() ? L"" : L", ") + g_hk[i].text;
    }
    return taken;
}
static void unregisterHotkeys() {
    for (int i = 0; i < 4; i++) UnregisterHotKey(g_main, i + 1);
}

static WORD toHotkeyBox(const Hotkey& h) { // value for a hotkey box (it has no Win key)
    if (!h.vk) return 0;
    BYTE f = 0;
    if (h.mods & MOD_SHIFT) f |= HOTKEYF_SHIFT;
    if (h.mods & MOD_CONTROL) f |= HOTKEYF_CONTROL;
    if (h.mods & MOD_ALT) f |= HOTKEYF_ALT;
    for (auto& k : kNamedKeys) if (k.vk == h.vk && h.vk != VK_PAUSE && h.vk != VK_SCROLL) f |= HOTKEYF_EXT;
    return MAKEWORD(h.vk, f);
}

static void comboAdd(HWND combo, const wchar_t* text, int value, int current) {
    int i = int(SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text));
    SendMessageW(combo, CB_SETITEMDATA, i, value);
    if (value == current) SendMessageW(combo, CB_SETCURSEL, i, 0);
}
static int comboValue(int id, int fallback) {
    HWND c = GetDlgItem(g_settings, id);
    int i = int(SendMessageW(c, CB_GETCURSEL, 0, 0));
    return i < 0 ? fallback : int(SendMessageW(c, CB_GETITEMDATA, i, 0));
}

// The folder picker needs its own thread (this program's main thread is not set up for it).
static DWORD WINAPI browseThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW bi{};
    bi.hwndOwner = g_settings;
    bi.lpszTitle = L"Folder for SnapCap recordings";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path) && g_settings) SetDlgItemTextW(g_settings, ID_FOLDER, path);
        CoTaskMemFree(pidl);
    }
    CoUninitialize();
    return 0;
}

static void settingsSave() {
    g_follow = IsDlgButtonChecked(g_settings, ID_FOLLOW) == BST_CHECKED;
    g_showOnScreen = IsDlgButtonChecked(g_settings, ID_SCREEN) == BST_CHECKED;
    g_showInVideo = IsDlgButtonChecked(g_settings, ID_VIDEO) == BST_CHECKED;
    g_askAdmin = IsDlgButtonChecked(g_settings, ID_ADMIN) == BST_CHECKED;
    g_forceBorderless = IsDlgButtonChecked(g_settings, ID_BORDERLESS) == BST_CHECKED;
    g_corner = comboValue(ID_CORNER, g_corner);
    for (int i = 0; i < 4; i++) {
        WORD v = LOWORD(SendDlgItemMessageW(g_settings, ID_HK0 + i, HKM_GETHOTKEY, 0, 0));
        if (v == g_hkShown[i]) continue; // untouched: keep it exactly (it may use the Win key)
        Hotkey h;
        h.vk = LOBYTE(v);
        BYTE f = HIBYTE(v);
        if (f & HOTKEYF_SHIFT) h.mods |= MOD_SHIFT;
        if (f & HOTKEYF_CONTROL) h.mods |= MOD_CONTROL;
        if (f & HOTKEYF_ALT) h.mods |= MOD_ALT;
        h.text = hotkeyText(h.mods, h.vk);
        g_hk[i] = h;
        g_hkShown[i] = v;
    }
    g_recOpt.fps = comboValue(ID_FPS, g_recOpt.fps);
    g_recOpt.maxHeight = comboValue(ID_SIZE, g_recOpt.maxHeight);
    g_recOpt.bitrateMbps = comboValue(ID_RATE, g_recOpt.bitrateMbps);
    g_recOpt.corner = g_corner;
    wchar_t folder[MAX_PATH] = L"";
    GetDlgItemTextW(g_settings, ID_FOLDER, folder, MAX_PATH);
    g_recOpt.folder = folder;
    while (!g_recOpt.folder.empty() && g_recOpt.folder.back() == L' ') g_recOpt.folder.pop_back();

    saveSettings();
    rec::setShowInVideo(g_showInVideo);
    rec::setOptions(g_recOpt); // used by the next recording
    refreshText();
    positionOverlay(true);
    // Try the hotkeys now so a clash is reported here, then release them again:
    // while this window is open they stay off so they can be typed into the boxes.
    std::wstring taken = registerHotkeys();
    unregisterHotkeys();
    SetDlgItemTextW(g_settings, ID_STATUS, taken.empty() ? L"Saved." : (L"Saved, but another program already uses " + taken + L".").c_str());
}

static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) settingsSave();
        else if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
        else if (LOWORD(w) == ID_BROWSE) { if (HANDLE t = CreateThread(nullptr, 0, browseThread, nullptr, 0, nullptr)) CloseHandle(t); }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: {
        g_settings = nullptr;
        std::wstring taken = registerHotkeys(); // hotkeys work again
        if (!taken.empty()) trayBalloon((L"Another program already uses " + taken + L". Use the tray menu for that, or choose another hotkey in Settings.").c_str());
        if (g_follow) followFocus(GetForegroundWindow());
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void openSettings() {
    if (g_settings) { ShowWindow(g_settings, SW_RESTORE); SetForegroundWindow(g_settings); return; }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.hInstance = g_inst;
        wc.lpfnWndProc = settingsProc;
        wc.lpszClassName = L"SnapCapSettings";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = g_icon;
        RegisterClassW(&wc);
        registered = true;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"SnapCapSettings", L"SnapCap settings", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, g_inst, nullptr);
    if (!wnd) return;
    g_settings = wnd;
    unregisterHotkeys(); // so the current hotkeys can be pressed into the boxes without firing
    const UINT dpi = GetDpiForWindow(wnd);
    auto S = [&](int v) { return MulDiv(v, dpi, 96); };
    if (g_setFont) DeleteObject(g_setFont);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    g_setFont = CreateFontIndirectW(&ncm.lfMessageFont);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, S(x), S(y), S(cw), S(ch), wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_setFont, TRUE);
        return c;
    };
    const int L = 18, CX = 200, W = 540, CW = W - CX - L;
    int y = 16;
    auto check = [&](const wchar_t* text, int id, bool on) {
        mk(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, L, y, W - 2 * L, 22, id);
        CheckDlgButton(wnd, id, on ? BST_CHECKED : BST_UNCHECKED);
        y += 26;
    };
    auto label = [&](const wchar_t* text) { mk(L"STATIC", text, 0, L, y + 4, CX - L - 8, 20, -1); };
    auto combo = [&](int id) { return mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, CX, y, CW, 240, id); };

    check(L"Follow the window in focus", ID_FOLLOW, g_follow);
    check(L"Show name and FPS on screen", ID_SCREEN, g_showOnScreen);
    check(L"Show name and FPS in the video", ID_VIDEO, g_showInVideo);
    check(L"Ask for exact FPS (administrator prompt) when SnapCap starts", ID_ADMIN, g_askAdmin);
    check(L"Force borderless mode (Fixes missing overlay in exclusive fullscreen games)", ID_BORDERLESS, g_forceBorderless);
    y += 8;
    label(L"Position of name and FPS");
    HWND c = combo(ID_CORNER);
    const wchar_t* corners[4] = {L"Top left", L"Top right", L"Bottom left", L"Bottom right"};
    for (int i = 0; i < 4; i++) comboAdd(c, corners[i], i, g_corner);
    y += 40;

    mk(L"STATIC", L"Hotkeys: click a box and press the keys. Backspace clears it.", 0, L, y, W - 2 * L, 20, -1);
    y += 26;
    const wchar_t* hkLabels[4] = {L"Stay on the window in front", L"Show or hide on screen", L"Start or stop recording", L"Show or hide in the video"};
    for (int i = 0; i < 4; i++) {
        label(hkLabels[i]);
        HWND hk = mk(HOTKEY_CLASSW, L"", WS_TABSTOP, CX, y, CW, 24, ID_HK0 + i, WS_EX_CLIENTEDGE);
        g_hkShown[i] = toHotkeyBox(g_hk[i]);
        SendMessageW(hk, HKM_SETHOTKEY, g_hkShown[i], 0);
        y += 30;
    }
    y += 10;

    mk(L"STATIC", L"Recording (used by the next recording)", 0, L, y, W - 2 * L, 20, -1);
    y += 26;
    label(L"Frames per second");
    c = combo(ID_FPS);
    comboAdd(c, L"60", 60, g_recOpt.fps);
    comboAdd(c, L"30", 30, g_recOpt.fps);
    y += 30;
    label(L"Video size");
    c = combo(ID_SIZE);
    comboAdd(c, L"Same as the window", 0, g_recOpt.maxHeight);
    for (int hgt : {2160, 1440, 1080, 720}) {
        wchar_t t[48];
        swprintf(t, 48, L"Up to %dp", hgt);
        comboAdd(c, t, hgt, g_recOpt.maxHeight);
    }
    if (SendMessageW(c, CB_GETCURSEL, 0, 0) < 0) { // a value typed into the file by hand
        wchar_t t[48];
        swprintf(t, 48, L"Up to %dp", g_recOpt.maxHeight);
        comboAdd(c, t, g_recOpt.maxHeight, g_recOpt.maxHeight);
    }
    y += 30;
    label(L"Quality");
    c = combo(ID_RATE);
    comboAdd(c, L"Automatic (about 12 Mbit/s at 1080p, 50 at 4K)", 0, g_recOpt.bitrateMbps);
    for (int mb : {50, 30, 20, 12, 8, 5}) {
        wchar_t t[48];
        swprintf(t, 48, L"%d Mbit/s (about %d MB a minute)", mb, mb * 60 / 8);
        comboAdd(c, t, mb, g_recOpt.bitrateMbps);
    }
    if (SendMessageW(c, CB_GETCURSEL, 0, 0) < 0) {
        wchar_t t[48];
        swprintf(t, 48, L"%d Mbit/s", g_recOpt.bitrateMbps);
        comboAdd(c, t, g_recOpt.bitrateMbps, g_recOpt.bitrateMbps);
    }
    y += 30;
    label(L"Save recordings in");
    mk(L"EDIT", g_recOpt.folder.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, CX, y, CW - 84, 24, ID_FOLDER, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, CX + CW - 78, y - 1, 78, 26, ID_BROWSE);
    y += 28;
    mk(L"STATIC", L"Leave empty for Videos\\SnapCap.", 0, CX, y, CW, 20, -1);
    y += 34;

    mk(L"STATIC", L"", 0, L, y + 5, W - 2 * L - 190, 36, ID_STATUS);
    mk(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, W - L - 176, y, 84, 28, IDOK);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, W - L - 84, y, 84, 28, IDCANCEL);
    y += 28 + 16;

    RECT r{0, 0, S(W), S(y)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dpi);
    SetWindowPos(wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
}
