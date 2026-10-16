// Hawk OS - physical input. Every call here acts on the real machine:
// the real cursor moves, real clicks land, real keystrokes are typed into
// whatever window has focus.
#include "hawk.h"

#include <cstring>

namespace hawk {
namespace {

// A click that lands on a background window can be swallowed by Windows as
// the activation click: the window comes up, but the control never sees the
// press. Multi-step flows (editor -> save dialog -> IDE) depend on every
// click landing, so bring the window under the cursor to the front first -
// the attention a person pays before they click. No-op when the window is
// already in front, so it costs nothing in the common case.
void raise_window_under_cursor() {
    Point p = cursor_pos();
    HWND wnd = WindowFromPoint(POINT{p.x, p.y});
    if (!wnd) return;
    HWND root = GetAncestor(wnd, GA_ROOT);
    if (!root || root == GetForegroundWindow()) return;
    focus_window(root);
    Sleep(70);  // let the window finish coming up before the press arrives
}

}  // namespace

static volatile LONG g_stop = 0;
static Point g_origin{};
static bool g_origin_saved = false;

// ------------------------------------------------------------- safety bits --
bool abort_requested() {
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) return true;
    // Holding ESC is the panic gesture: it stops a run between two steps.
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
}

void request_abort(bool on) { InterlockedExchange(&g_stop, on ? 1 : 0); }

void save_cursor_origin() {
    g_origin = cursor_pos();
    g_origin_saved = true;
}

void restore_cursor_origin() {
    if (!g_origin_saved) return;
    SetCursorPos(g_origin.x, g_origin.y);
    g_origin_saved = false;
}

Point cursor_pos() {
    POINT p{};
    GetCursorPos(&p);
    return Point{p.x, p.y};
}

// ----------------------------------------------------------------- helpers --
static bool send(const INPUT& in) { return SendInput(1, (INPUT*)&in, sizeof(INPUT)) == 1; }

static bool within(Point a, int x, int y, int tol = 3) {
    return abs(a.x - x) <= tol && abs(a.y - y) <= tol;
}

uint32_t pixel_at(int x, int y) {
    HDC dc = GetDC(nullptr);
    if (!dc) return 0xFFFFFFFFu;
    COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return 0xFFFFFFFFu;
    return (uint32_t)c;
}

// -------------------------------------------------------------- the cursor --
bool move_cursor(int x, int y, std::string* err, Point* landed) {
    enable_dpi_awareness();
    Rect v = virtual_desktop();
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    // Absolute mouse coordinates are normalised over the whole virtual desktop.
    in.mi.dx = (LONG)(((double)(x - v.x) * 65535.0) / (v.w > 1 ? v.w - 1 : 1) + 0.5);
    in.mi.dy = (LONG)(((double)(y - v.y) * 65535.0) / (v.h > 1 ? v.h - 1 : 1) + 0.5);
    send(in);

    Point p = cursor_pos();
    if (!within(p, x, y)) {
        // Some sessions swallow the normalised form (RDP, certain hooks).
        SetCursorPos(x, y);
        p = cursor_pos();
    }
    if (landed) *landed = p;
    if (!within(p, x, y)) {
        if (err)
            *err = "cursor landed at " + std::to_string(p.x) + "," + std::to_string(p.y) +
                   " (wanted " + std::to_string(x) + "," + std::to_string(y) + ")";
        return false;
    }
    return true;
}

bool move_cursor_relative(int dx, int dy, std::string* err, Point* landed) {
    Point before = cursor_pos();
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    send(in);
    Point after = cursor_pos();
    int wx = before.x + dx, wy = before.y + dy;
    if (!within(after, wx, wy)) {
        SetCursorPos(wx, wy);
        if (err) *err = "clamped to screen edge";
        after = cursor_pos();
    }
    if (landed) *landed = after;
    return true;
}

// ---------------------------------------------------------------- clicking --
static void send_button(const char* button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    std::string b = to_lower(button ? button : "left");
    if (b == "right") {
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (b == "middle") {
        in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    } else {
        in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    }
    send(in);
}

bool click_mouse(const std::string& button, int times, std::string* err) {
    if (abort_requested()) {
        if (err) *err = "aborted before click";
        return false;
    }
    raise_window_under_cursor();
    if (times < 1) times = 1;
    if (times > 3) times = 3;
    for (int i = 0; i < times; i++) {
        send_button(button.c_str(), true);
        Sleep(12);
        send_button(button.c_str(), false);
        if (i + 1 < times) Sleep(38);  // a double click must arrive quickly
    }
    return true;
}

bool drag_mouse(int x1, int y1, int x2, int y2, const std::string& button, std::string* err) {
    if (!move_cursor(x1, y1, err)) return false;
    raise_window_under_cursor();
    Sleep(30);
    send_button(button.c_str(), true);
    Sleep(30);
    int steps = 24;
    for (int i = 1; i <= steps; i++) {
        if (abort_requested()) {
            send_button(button.c_str(), false);
            if (err) *err = "aborted mid-drag";
            return false;
        }
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        SetCursorPos(x, y);
        Sleep(8);
    }
    Sleep(30);
    send_button(button.c_str(), false);
    Point p = cursor_pos();
    if (!within(p, x2, y2)) {
        if (err) *err = "drag ended at " + std::to_string(p.x) + "," + std::to_string(p.y);
        return false;
    }
    return true;
}

bool scroll_wheel(int notches, std::string* err) {
    if (notches == 0) return true;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(notches * WHEEL_DELTA);
    if (!send(in)) {
        if (err) *err = "SendInput refused the wheel event";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- keyboard --
static bool type_one_wchar(wchar_t ch) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = ch;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    return SendInput(2, in, sizeof(INPUT)) == 2;
}

bool type_text(const std::wstring& text, std::string* err) {
    // Newlines are typed as a real Enter key so editors treat them normally.
    INPUT nl[2]{};
    nl[0].type = INPUT_KEYBOARD;
    nl[0].ki.wVk = VK_RETURN;
    nl[1] = nl[0];
    nl[1].ki.dwFlags = KEYEVENTF_KEYUP;

    for (size_t i = 0; i < text.size(); i++) {
        if (abort_requested()) {
            if (err) *err = "aborted while typing";
            return false;
        }
        wchar_t ch = text[i];
        if (ch == L'\n') {
            SendInput(2, nl, sizeof(INPUT));
        } else if (ch == L'\t') {
            nl[0].ki.wVk = VK_TAB;
            nl[1].ki.wVk = VK_TAB;
            SendInput(2, nl, sizeof(INPUT));
            nl[0].ki.wVk = VK_RETURN;
            nl[1].ki.wVk = VK_RETURN;
        } else if (ch == L'\r') {
            continue;
        } else {
            if (!type_one_wchar(ch)) {
                if (err) *err = "SendInput refused a keystroke";
                return false;
            }
        }
        // A few milliseconds between keys: fast enough to be quick, slow
        // enough that a busy application (a heavy editor mid-startup, say)
        // keeps up and no keystroke is dropped or doubled.
        Sleep(5);
    }
    return true;
}

static WORD vk_for_name(const std::string& raw, bool* needs_shift) {
    std::string n = to_lower(trim(raw));
    *needs_shift = false;
    if (n == "ctrl" || n == "control") return VK_CONTROL;
    if (n == "shift") return VK_SHIFT;
    if (n == "alt") return VK_MENU;
    if (n == "win" || n == "super" || n == "meta") return VK_LWIN;
    if (n == "enter" || n == "return") return VK_RETURN;
    if (n == "tab") return VK_TAB;
    if (n == "esc" || n == "escape") return VK_ESCAPE;
    if (n == "space") return VK_SPACE;
    if (n == "backspace" || n == "back") return VK_BACK;
    if (n == "delete" || n == "del") return VK_DELETE;
    if (n == "insert") return VK_INSERT;
    if (n == "home") return VK_HOME;
    if (n == "end") return VK_END;
    if (n == "pageup" || n == "pgup") return VK_PRIOR;
    if (n == "pagedown" || n == "pgdn") return VK_NEXT;
    if (n == "up") return VK_UP;
    if (n == "down") return VK_DOWN;
    if (n == "left") return VK_LEFT;
    if (n == "right") return VK_RIGHT;
    if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1])) {
        int k = atoi(n.c_str() + 1);
        if (k >= 1 && k <= 24) return (WORD)(VK_F1 + k - 1);
    }
    if (n.size() == 1) {
        SHORT v = VkKeyScanW((wchar_t)n[0]);
        if (v != -1) {
            if (v & 0x100) *needs_shift = true;
            return (WORD)(v & 0xFF);
        }
    }
    return 0;
}

bool press_combo(const std::string& combo, std::string* err) {
    // "ctrl+shift+s", "enter", "alt+f4" - modifiers are held around the key.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : combo) {
        if (c == '+') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    if (parts.empty() || trim(parts.back()).empty()) {
        if (err) *err = "no key given";
        return false;
    }

    bool shift_extra = false;
    WORD key = vk_for_name(parts.back(), &shift_extra);
    if (!key) {
        if (err) *err = "unknown key '" + parts.back() + "'";
        return false;
    }

    std::vector<WORD> mods;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        bool s = false;
        WORD m = vk_for_name(parts[i], &s);
        if (!m) {
            if (err) *err = "unknown modifier '" + parts[i] + "'";
            return false;
        }
        mods.push_back(m);
    }
    if (shift_extra) mods.push_back(VK_SHIFT);

    std::vector<INPUT> seq;
    auto key_event = [](WORD vk, bool up) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        return in;
    };
    for (WORD m : mods) seq.push_back(key_event(m, false));
    seq.push_back(key_event(key, false));
    seq.push_back(key_event(key, true));
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) seq.push_back(key_event(*it, true));

    if (SendInput((UINT)seq.size(), seq.data(), sizeof(INPUT)) != seq.size()) {
        if (err) *err = "SendInput refused the key sequence";
        return false;
    }
    return true;
}

}  // namespace hawk
