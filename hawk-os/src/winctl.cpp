// Hawk OS - real window control: see what is on screen, bring a window forward.
#include "hawk.h"

#include <dwmapi.h>
#include <cstdio>

namespace hawk {

static std::wstring process_of(DWORD pid) {
    if (!pid) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        std::wstring full(buf, n);
        size_t slash = full.find_last_of(L"\\/");
        out = slash == std::wstring::npos ? full : full.substr(slash + 1);
    }
    CloseHandle(h);
    return out;
}

static Rect rect_of(HWND hwnd) {
    RECT rc{};
    if (FAILED(DwmGetWindowAttribute(hwnd, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &rc, sizeof(rc))))
        GetWindowRect(hwnd, &rc);
    return Rect{rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top};
}

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM data) {
    auto* out = reinterpret_cast<std::vector<WinInfo>*>(data);
    if (out->size() >= 400) return FALSE;

    int len = GetWindowTextLengthW(hwnd);
    if (len > 0) {
        std::wstring title((size_t)len + 1, L'\0');
        int n = GetWindowTextW(hwnd, title.data(), len + 1);
        title.resize(n > 0 ? n : 0);

        wchar_t cls[256] = {0};
        GetClassNameW(hwnd, cls, 255);

        WinInfo info;
        info.hwnd = hwnd;
        info.title = title;
        info.cls = cls;
        info.rect = rect_of(hwnd);
        info.visible = IsWindowVisible(hwnd) != 0;
        info.minimized = IsIconic(hwnd) != 0;
        GetWindowThreadProcessId(hwnd, &info.pid);
        info.process = process_of(info.pid);
        out->push_back(info);
    }
    return TRUE;
}

std::vector<WinInfo> list_windows(size_t limit) {
    enable_dpi_awareness();
    std::vector<WinInfo> all;
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&all));

    // visible, un-minimised windows with a real size first: those are the ones
    // an agent can actually interact with.
    std::vector<WinInfo> ranked;
    for (const auto& w : all) {
        if (w.visible && !w.minimized && w.rect.w > 40 && w.rect.h > 40) ranked.push_back(w);
    }
    for (const auto& w : all) {
        bool already = false;
        for (const auto& r : ranked)
            if (r.hwnd == w.hwnd) already = true;
        if (!already) ranked.push_back(w);
    }
    if (limit && ranked.size() > limit) ranked.resize(limit);
    return ranked;
}

std::wstring window_title(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring title((size_t)len + 1, L'\0');
    int n = GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(n > 0 ? n : 0);
    return title;
}

static bool contains_ci(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    std::wstring h = hay, n = needle;
    for (auto& c : h) c = towlower(c);
    for (auto& c : n) c = towlower(c);
    return h.find(n) != std::wstring::npos;
}

HWND find_window(const std::string& needle) {
    std::wstring want = wide(needle);
    HWND fallback = nullptr;
    for (const auto& w : list_windows(0)) {
        if (!contains_ci(w.title, want)) continue;
        if (w.visible && !w.minimized && w.rect.w > 40) return w.hwnd;
        if (!fallback) fallback = w.hwnd;
    }
    return fallback;
}

bool focus_window(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    HWND fg = GetForegroundWindow();
    DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = false;
    if (fg_thread && fg_thread != me) attached = AttachThreadInput(me, fg_thread, TRUE) != 0;

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (attached) AttachThreadInput(me, fg_thread, FALSE);

    for (int i = 0; i < 20; i++) {
        if (GetForegroundWindow() == hwnd) return true;
        Sleep(10);
    }
    return false;
}

std::string describe_windows(size_t limit) {
    std::string out;
    auto wins = list_windows(limit);
    char line[512];
    for (size_t i = 0; i < wins.size(); i++) {
        const auto& w = wins[i];
        snprintf(line, sizeof(line),
                 "%2zu  pid=%-6lu %4dx%-4d @%-6d,%-6d %s%s  %s  [%s]",
                 i, (unsigned long)w.pid, w.rect.w, w.rect.h, w.rect.x, w.rect.y,
                 w.visible ? "vis" : "hid", w.minimized ? " min" : "    ",
                 narrow(w.title).c_str(), narrow(w.process).c_str());
        out += line;
        out += "\n";
    }
    if (out.empty()) out = "(no titled windows)\n";
    return out;
}

}  // namespace hawk
