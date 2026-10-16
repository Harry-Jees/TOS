// Hawk OS - screen capture. Real pixels, from the real machine.
#include "hawk.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>

namespace hawk {

static bool g_dpi_done = false;

void enable_dpi_awareness() {
    if (g_dpi_done) return;
    g_dpi_done = true;
    // Prefer per-monitor v2 so screen coordinates are physical pixels at every
    // scaling factor; that keeps injected input aligned with captured pixels.
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (fn && fn(reinterpret_cast<HANDLE>(-4) /* PER_MONITOR_AWARE_V2 */)) return;
    }
    SetProcessDPIAware();
}

Rect virtual_desktop() {
    enable_dpi_awareness();
    return Rect{GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

static BOOL CALLBACK monitor_proc(HMONITOR hmon, HDC, LPRECT, LPARAM data) {
    auto* out = reinterpret_cast<std::vector<Rect>*>(data);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        out->push_back(Rect{mi.rcMonitor.left, mi.rcMonitor.top,
                            mi.rcMonitor.right - mi.rcMonitor.left,
                            mi.rcMonitor.bottom - mi.rcMonitor.top});
    }
    return TRUE;
}

std::vector<Rect> monitor_rects() {
    enable_dpi_awareness();
    std::vector<Rect> out;
    EnumDisplayMonitors(nullptr, nullptr, monitor_proc, reinterpret_cast<LPARAM>(&out));
    return out;
}

// Capture a rectangle of the virtual desktop into a 32-bit top-down buffer.
static Image grab(const Rect& r) {
    Image img;
    if (r.w <= 0 || r.h <= 0) return img;

    HDC screen = GetDC(nullptr);
    if (!screen) return img;
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = r.w;
    bmi.bmiHeader.biHeight = -r.h;  // negative: rows run top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        // CAPTUREBLT folds in layered windows so floating panels are included.
        BitBlt(mem, 0, 0, r.w, r.h, screen, r.x, r.y, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        img.w = r.w;
        img.h = r.h;
        img.px.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)r.w * r.h * 4);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

Image capture_screen() {
    Rect v = virtual_desktop();
    Image img = grab(v);
    if (img.empty()) {
        // extremely defensive: fall back to the primary monitor metrics
        img = grab(Rect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)});
    }
    return img;
}

Image capture_region(const Rect& r) {
    Rect v = virtual_desktop();
    Rect clipped = r;
    if (clipped.x < v.x) {
        clipped.w -= (v.x - clipped.x);
        clipped.x = v.x;
    }
    if (clipped.y < v.y) {
        clipped.h -= (v.y - clipped.y);
        clipped.y = v.y;
    }
    if (clipped.x + clipped.w > v.x + v.w) clipped.w = v.x + v.w - clipped.x;
    if (clipped.y + clipped.h > v.y + v.h) clipped.h = v.y + v.h - clipped.y;
    if (clipped.w <= 0 || clipped.h <= 0) return Image{};

    HDC screen = GetDC(nullptr);
    if (!screen) return Image{};
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = clipped.w;
    bmi.bmiHeader.biHeight = -clipped.h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    Image img;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, clipped.w, clipped.h, screen, clipped.x, clipped.y, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        img.w = clipped.w;
        img.h = clipped.h;
        img.px.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)clipped.w * clipped.h * 4);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

Image capture_window(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return Image{};
    enable_dpi_awareness();

    // Real visible bounds, excluding the invisible resize border / shadow.
    RECT rc{};
    HRESULT hr = DwmGetWindowAttribute(hwnd, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/,
                                       &rc, sizeof(rc));
    if (FAILED(hr)) GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return Image{};

    HDC screen = GetDC(nullptr);
    if (!screen) return Image{};
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    Image img;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        // PrintWindow renders the window even when it is covered or offscreen.
        BOOL ok = PrintWindow(hwnd, mem, 2 /*PW_RENDERFULLCONTENT*/);
        if (!ok) BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        img.w = w;
        img.h = h;
        img.px.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)w * h * 4);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return img;
}

// -------------------------------------------------------------- shot files --
std::string shot_dir() {
    std::string dir = app_dir() + "\\shots";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string take_shot(const std::string& name_hint) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string hint = name_hint.empty() ? "screen" : name_hint;
    for (char& c : hint)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '-';
    char name[256];
    snprintf(name, sizeof(name), "%s\\shot-%04d%02d%02d-%02d%02d%02d-%s.png",
             shot_dir().c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, hint.c_str());
    Image img = capture_screen();
    if (!save_png(img, name)) return "";
    return name;
}

}  // namespace hawk
