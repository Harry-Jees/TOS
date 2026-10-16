// Hawk OS - the app window.
//
// One dark window: a live view of the real screen on the left, the Agent on
// the right (goal box, script box, run controls, log). The Agent runs on a
// worker thread so the window keeps painting and STOP always responds.
#include "hawk.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace hawk {

// --------------------------------------------------------------- constants --
enum {
    ID_GOAL = 1001,
    ID_SCRIPT,
    ID_LOG,
    ID_BTN_GOAL = 1010,
    ID_BTN_SCRIPT,
    ID_BTN_STOP,
    ID_BTN_CAPTURE,
    ID_BTN_FOLDER,
    ID_BTN_SETTINGS,
    ID_BTN_CLEAR,
    ID_CHK_DRYRUN,
    ID_CHK_CLICKTHRU,
    ID_CHK_RESTORE,
    ID_CHK_AUTORUN,
    ID_QUICK1,
    ID_QUICK2,
    ID_QUICK3,
    ID_QUICK4,
    ID_STATUS,
    ID_HOTKEY_STOP = 1,
};

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_SCRIPT (WM_APP + 3)

static const int HEADER_H = 62;
#define HEADER_TOP() (HEADER_H + 10)

static const COLORREF C_BG = RGB(22, 22, 26);
static const COLORREF C_PANEL = RGB(30, 30, 35);
static const COLORREF C_HEADER1 = RGB(44, 44, 52);
static const COLORREF C_HEADER2 = RGB(26, 26, 31);
static const COLORREF C_TEXT = RGB(236, 236, 242);
static const COLORREF C_MUTED = RGB(148, 148, 158);
static const COLORREF C_ACCENT = RGB(10, 132, 255);
static const COLORREF C_DANGER = RGB(214, 62, 54);
static const COLORREF C_BTN = RGB(54, 54, 62);
static const COLORREF C_BTN_HI = RGB(70, 70, 80);
static const COLORREF C_LINE = RGB(60, 60, 68);
static const COLORREF C_FIELD = RGB(17, 17, 20);

struct UiState {
    HWND hwnd = nullptr;
    HWND goal = nullptr, script = nullptr, log = nullptr, status = nullptr;
    HWND btn_goal = nullptr, btn_script = nullptr, btn_stop = nullptr, btn_capture = nullptr;
    HWND btn_folder = nullptr, btn_settings = nullptr, btn_clear = nullptr;
    HWND chk_dry = nullptr, chk_click = nullptr, chk_restore = nullptr, chk_autorun = nullptr;
    HWND quick1 = nullptr, quick2 = nullptr, quick3 = nullptr, quick4 = nullptr;

    HFONT f_ui = nullptr, f_bold = nullptr, f_title = nullptr, f_mono = nullptr;
    HBRUSH b_bg = nullptr, b_panel = nullptr, b_field = nullptr, b_log = nullptr;
    Image preview;
    Rect preview_rect{};      // where the screen image is drawn (client coords)
    double preview_scale = 1.0;
    Point last_click{-1, -1};
    DWORD suppress_click_until = 0;
    bool click_through = false;
    bool dry_run = false;
    bool auto_run = true;
    bool follow_log = true;
    std::string status_text = "idle";
    std::string last_shot;
};

static UiState* g_ui = nullptr;
static Config g_cfg;
static std::atomic<bool> g_running{false};
static std::thread g_worker;
static HANDLE g_hover = nullptr;

// ---------------------------------------------------------------- helpers ---
static void set_font(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

static HWND make_button(HWND parent, int id, const wchar_t* text, HFONT font,
                        const std::wstring& tip = L"") {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    set_font(h, font);
    if (!tip.empty()) {
        // The tooltip control keeps a pointer to the text, so it has to live on.
        static std::vector<std::wstring*> kept;
        kept.push_back(new std::wstring(tip));
        static HWND tipw = nullptr;
        if (!tipw) {
            tipw = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, parent,
                                   nullptr, nullptr, nullptr);
        }
        if (tipw) {
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = parent;
            ti.uId = (UINT_PTR)h;
            ti.lpszText = const_cast<LPWSTR>(kept.back()->c_str());
            SendMessageW(tipw, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
    return h;
}

static void log_append(const LogLine& line) {
    if (!g_ui || !g_ui->log) return;
    wchar_t stamp[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(stamp, 32, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    std::wstring text = stamp + wide(line.text) + L"\r\n";
    int len = GetWindowTextLengthW(g_ui->log);
    if (g_ui->follow_log) {
        SendMessageW(g_ui->log, EM_SETSEL, len, len);
        SendMessageW(g_ui->log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
        SendMessageW(g_ui->log, EM_SCROLLCARET, 0, 0);
    }
}

static std::string box_text(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return "";
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize(len);
    return narrow(s);
}

static void set_status(const std::string& s) {
    if (g_ui) {
        g_ui->status_text = s;
        if (g_ui->status) SetWindowTextW(g_ui->status, wide(s).c_str());
    }
}

static void set_running(bool on) {
    g_running = on;
    if (!g_ui) return;
    EnableWindow(g_ui->btn_goal, !on);
    EnableWindow(g_ui->btn_script, !on);
    EnableWindow(g_ui->quick1, !on);
    EnableWindow(g_ui->quick2, !on);
    EnableWindow(g_ui->quick3, !on);
    EnableWindow(g_ui->btn_stop, on);
    set_status(on ? "running - press STOP or hold ESC to abort" : "idle");
}

// ---------------------------------------------------------- worker thread ---
static void start_job(const std::string& kind, const std::string& payload) {
    if (g_running) {
        log_append(LogLine{"warn", "a run is already in progress"});
        return;
    }
    if (g_worker.joinable()) g_worker.join();

    Config cfg;
    cfg.dry_run = g_ui->dry_run;
    cfg.restore_cursor = SendMessageW(g_ui->chk_restore, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.api_key = g_cfg.api_key;
    cfg.base_url = g_cfg.base_url;
    cfg.model = g_cfg.model;
    cfg.step_delay_ms = g_cfg.step_delay_ms;
    cfg.auto_run = g_ui->auto_run;

    request_abort(false);
    set_running(true);
    g_worker = std::thread([kind, payload, cfg]() {
        Agent agent(cfg);
        agent.on_log = [](const LogLine& l) {
            // hop to the UI thread; the Edit control is not thread safe
            auto* copy = new LogLine(l);
            if (!PostMessageW(g_ui->hwnd, WM_APP_LOG, 0, (LPARAM)copy)) delete copy;
        };
        // A script the model (or the built-in planner) produced is shown in the
        // script box while the app runs it, so you can see and reuse it.
        agent.on_script = [](const std::string& script) {
            auto* copy = new std::string(script);
            if (!PostMessageW(g_ui->hwnd, WM_APP_SCRIPT, 0, (LPARAM)copy)) delete copy;
        };
        if (kind == "goal") agent.run_goal(payload);
        else agent.run_script(payload);
        PostMessageW(g_ui->hwnd, WM_APP_DONE, 0, 0);
    });
}

// ------------------------------------------------------------- preview ------
static void compute_preview_rect(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int left_w = (int)((rc.right - rc.left) * 0.62);
    if (left_w < 420) left_w = 420;
    int top = HEADER_TOP();
    int avail_w = left_w - 36;
    int avail_h = rc.bottom - top - 70;
    if (avail_w < 80) avail_w = 80;
    if (avail_h < 80) avail_h = 80;

    Rect v = virtual_desktop();
    double sx = avail_w / (double)v.w;
    double sy = avail_h / (double)v.h;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    int w = (int)(v.w * s), h = (int)(v.h * s);
    g_ui->preview_scale = s;
    g_ui->preview_rect = Rect{26, top + 26, w, h};
}

// --------------------------------------------------------------- painting ---
static void draw_round(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border, bool has_border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, has_border ? border : fill);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    // double buffer to keep the live view from flickering
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    // header with a soft vertical gradient
    RECT head{0, 0, rc.right, HEADER_H};
    TRIVERTEX verts[2] = {
        {0, 0, (COLOR16)(GetRValue(C_HEADER1) << 8), (COLOR16)(GetGValue(C_HEADER1) << 8),
         (COLOR16)(GetBValue(C_HEADER1) << 8), 0xFF00},
        {rc.right, HEADER_H, (COLOR16)(GetRValue(C_HEADER2) << 8), (COLOR16)(GetGValue(C_HEADER2) << 8),
         (COLOR16)(GetBValue(C_HEADER2) << 8), 0xFF00}};
    GRADIENT_RECT grad{0, 1};
    if (!GradientFill(mem, verts, 2, &grad, 1, GRADIENT_FILL_RECT_V)) {
        HBRUSH hb = CreateSolidBrush(C_HEADER1);
        FillRect(mem, &head, hb);
        DeleteObject(hb);
    }
    SetBkMode(mem, TRANSPARENT);

    HGDIOBJ oldf = SelectObject(mem, g_ui->f_title);
    SetTextColor(mem, C_TEXT);
    TextOutW(mem, 20, 12, L"Hawk OS", 7);
    SelectObject(mem, g_ui->f_ui);
    SetTextColor(mem, C_MUTED);
    std::wstring sub = L"desktop control agent  v" + wide(HAWK_VERSION) + L"   -   " +
                       wide(g_ui->status_text);
    TextOutW(mem, 20, 38, sub.c_str(), (int)sub.size());

    // left: the live screen
    Rect pr = g_ui->preview_rect;
    if (!g_ui->preview.empty() && pr.w > 10) {
        HBRUSH frame = CreateSolidBrush(RGB(12, 12, 14));
        RECT fr{pr.x - 2, pr.y - 2, pr.x + pr.w + 2, pr.y + pr.h + 2};
        FillRect(mem, &fr, frame);
        DeleteObject(frame);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = g_ui->preview.w;
        bmi.bmiHeader.biHeight = -g_ui->preview.h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        StretchDIBits(mem, pr.x, pr.y, pr.w, pr.h, 0, 0, g_ui->preview.w, g_ui->preview.h,
                      g_ui->preview.px.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

        Rect v = virtual_desktop();
        auto to_preview = [&](Point p) {
            return Point{pr.x + (int)((p.x - v.x) * g_ui->preview_scale),
                         pr.y + (int)((p.y - v.y) * g_ui->preview_scale)};
        };

        // mark the last injected click
        if (g_ui->last_click.x >= 0) {
            Point pp = to_preview(g_ui->last_click);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 70, 70));
            HGDIOBJ op = SelectObject(mem, pen);
            HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
            int r = 12;
            Ellipse(mem, pp.x - r, pp.y - r, pp.x + r, pp.y + r);
            MoveToEx(mem, pp.x - r - 4, pp.y, nullptr);
            LineTo(mem, pp.x + r + 4, pp.y);
            MoveToEx(mem, pp.x, pp.y - r - 4, nullptr);
            LineTo(mem, pp.x, pp.y + r + 4);
            SelectObject(mem, op);
            SelectObject(mem, ob);
            DeleteObject(pen);
        }

        // crosshair at the real cursor
        Point cur = cursor_pos();
        Point cp = to_preview(cur);
        HPEN cpen = CreatePen(PS_SOLID, 1, RGB(60, 240, 110));
        HGDIOBJ op = SelectObject(mem, cpen);
        MoveToEx(mem, cp.x - 10, cp.y, nullptr);
        LineTo(mem, cp.x + 10, cp.y);
        MoveToEx(mem, cp.x, cp.y - 10, nullptr);
        LineTo(mem, cp.x, cp.y + 10);
        SelectObject(mem, op);
        DeleteObject(cpen);

        // caption under the preview
        SelectObject(mem, g_ui->f_ui);
        SetTextColor(mem, C_MUTED);
        char cap[256];
        snprintf(cap, sizeof(cap),
                 "live screen %dx%d  shown at %.0f%%   cursor %d,%d   %s",
                 v.w, v.h, g_ui->preview_scale * 100.0, cur.x, cur.y,
                 g_ui->click_through ? "click-through ON" : "click-through off");
        TextOutA(mem, pr.x, pr.y + pr.h + 10, cap, (int)strlen(cap));
        if (g_ui->click_through) {
            SelectObject(mem, g_ui->f_bold);
            SetTextColor(mem, RGB(255, 190, 70));
            const wchar_t* warn = L"clicking the preview will really click your screen";
            TextOutW(mem, pr.x, pr.y + pr.h + 28, warn, (int)wcslen(warn));
        }
    }

    // right panel background
    int panel_x = pr.x + pr.w + 34;
    RECT panel{panel_x - 14, HEADER_TOP() + 8, rc.right - 16, rc.bottom - 14};
    draw_round(mem, panel, 14, C_PANEL, C_LINE, true);

    SelectObject(mem, g_ui->f_bold);
    SetTextColor(mem, C_TEXT);
    TextOutW(mem, panel_x, HEADER_TOP() + 20, L"Agent", 5);
    SelectObject(mem, g_ui->f_ui);
    SetTextColor(mem, g_cfg.api_key.empty() ? RGB(255, 190, 70) : RGB(70, 200, 120));
    const wchar_t* key_state = g_cfg.api_key.empty() ? L"no model key - script mode only"
                                                    : L"model key set";
    TextOutW(mem, panel_x + 60, HEADER_TOP() + 21, key_state, (int)wcslen(key_state));
    if (!g_ui->last_shot.empty()) {
        std::wstring shot = L"last shot: " + wide(g_ui->last_shot);
        SetTextColor(mem, C_MUTED);
        TextOutW(mem, panel_x, rc.bottom - 28, shot.c_str(),
                 (int)shot.size() > 96 ? 96 : (int)shot.size());
    }

    SelectObject(mem, oldf);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- layout ----
static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    compute_preview_rect(hwnd);
    Rect pr = g_ui->preview_rect;
    int x = pr.x + pr.w + 34;
    int w = rc.right - 16 - x;
    if (w < 300) w = 300;
    int y = HEADER_TOP() + 48;

    MoveWindow(g_ui->goal, x, y, w, 26, TRUE);
    y += 34;
    MoveWindow(g_ui->btn_goal, x, y, 96, 28, TRUE);
    MoveWindow(g_ui->btn_capture, x + 102, y, 88, 28, TRUE);
    MoveWindow(g_ui->btn_stop, x + 196, y, 76, 28, TRUE);
    MoveWindow(g_ui->btn_settings, x + 278, y, 74, 28, TRUE);
    y += 36;
    MoveWindow(g_ui->chk_dry, x, y, 112, 20, TRUE);
    MoveWindow(g_ui->chk_restore, x + 112, y, 140, 20, TRUE);
    MoveWindow(g_ui->chk_autorun, x + 256, y, 130, 20, TRUE);
    y += 26;
    MoveWindow(g_ui->quick1, x, y, 96, 24, TRUE);
    MoveWindow(g_ui->quick2, x + 102, y, 96, 24, TRUE);
    MoveWindow(g_ui->quick3, x + 204, y, 96, 24, TRUE);
    MoveWindow(g_ui->quick4, x + 306, y, 96, 24, TRUE);
    y += 32;

    int bottom = rc.bottom - 40;
    int script_h = 108;
    int log_h = bottom - (y + 22 + script_h + 26);
    if (log_h < 70) log_h = 70;
    MoveWindow(g_ui->script, x, y + 20, w, script_h, TRUE);
    MoveWindow(g_ui->btn_script, x, y + 20 + script_h + 4, 92, 24, TRUE);
    MoveWindow(g_ui->btn_clear, x + 98, y + 20 + script_h + 4, 62, 24, TRUE);
    MoveWindow(g_ui->chk_click, x + 168, y + 20 + script_h + 4, 150, 20, TRUE);
    MoveWindow(g_ui->btn_folder, x + w - 96, y + 20 + script_h + 4, 96, 24, TRUE);
    MoveWindow(g_ui->log, x, y + 22 + script_h + 32, w, log_h, TRUE);
}

// Publish the window and control geometry in screen pixels. This is what lets
// tooling (and another Hawk OS run) click the app's own buttons by coordinate.
static void dump_layout(HWND hwnd) {
    if (!g_ui) return;
    if (IsIconic(hwnd)) return;  // a minimized window has no useful geometry
    FILE* f = fopen((app_dir() + "\\ui-layout.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "# Hawk OS window geometry in screen pixels (regenerated on resize)\n");
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    fprintf(f, "window %ld %ld %ld %ld\n", wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
    RECT cr{};
    GetClientRect(hwnd, &cr);
    POINT origin{cr.left, cr.top};
    ClientToScreen(hwnd, &origin);
    fprintf(f, "client %ld %ld %ld %ld\n", (long)origin.x, (long)origin.y,
            (long)(cr.right - cr.left), (long)(cr.bottom - cr.top));
    struct Item {
        const char* name;
        HWND h;
    } items[] = {
        {"goal", g_ui->goal},           {"script", g_ui->script},
        {"log", g_ui->log},             {"run_goal", g_ui->btn_goal},
        {"run_script", g_ui->btn_script}, {"stop", g_ui->btn_stop},
        {"capture", g_ui->btn_capture}, {"settings", g_ui->btn_settings},
        {"shots_folder", g_ui->btn_folder}, {"clear", g_ui->btn_clear},
        {"dry_run", g_ui->chk_dry},     {"click_through", g_ui->chk_click},
        {"auto_run", g_ui->chk_autorun}, {"hello_harry", g_ui->quick3},
        {"notepad_demo", g_ui->quick2}, {"help", g_ui->quick4},
    };
    for (const Item& it : items) {
        if (!it.h) continue;
        RECT r{};
        GetWindowRect(it.h, &r);
        fprintf(f, "%s %ld %ld %ld %ld\n", it.name, r.left, r.top, r.right - r.left,
                r.bottom - r.top);
    }
    POINT p{g_ui->preview_rect.x, g_ui->preview_rect.y};
    ClientToScreen(hwnd, &p);
    fprintf(f, "preview %ld %ld %d %d\n", (long)p.x, (long)p.y, g_ui->preview_rect.w,
            g_ui->preview_rect.h);
    fclose(f);
}

// ------------------------------------------------------------ ownerdraw -----
static void draw_button(LPDRAWITEMSTRUCT di) {
    bool is_goal = di->CtlID == ID_BTN_GOAL;
    bool is_stop = di->CtlID == ID_BTN_STOP;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    bool hot = g_hover == di->hwndItem;

    COLORREF fill = C_BTN;
    if (is_goal) fill = pressed ? RGB(0, 100, 200) : C_ACCENT;
    else if (is_stop) fill = pressed ? RGB(170, 40, 34) : C_DANGER;
    else fill = pressed ? RGB(44, 44, 52) : (hot ? C_BTN_HI : C_BTN);
    if (disabled) fill = RGB(42, 42, 48);

    RECT r = di->rcItem;
    draw_round(di->hDC, r, 8, fill, C_LINE, !is_goal && !is_stop);

    wchar_t text[64] = {0};
    GetWindowTextW(di->hwndItem, text, 63);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? C_MUTED : C_TEXT);
    HGDIOBJ oldf = SelectObject(di->hDC, g_ui->f_ui);
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(di->hDC, oldf);
}

// --------------------------------------------------------------- settings ---
static HWND g_cfg_wnd = nullptr, g_cfg_key = nullptr, g_cfg_url = nullptr, g_cfg_model = nullptr;
static bool g_cfg_saved = false;

static LRESULT CALLBACK cfg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_cfg_key = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_PASSWORD,
                                        20, 62, 420, 26, hwnd, nullptr, nullptr, nullptr);
            g_cfg_url = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wide(g_cfg.base_url).c_str(),
                                        WS_CHILD | WS_VISIBLE, 20, 128, 420, 26, hwnd, nullptr,
                                        nullptr, nullptr);
            g_cfg_model = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wide(g_cfg.model).c_str(),
                                          WS_CHILD | WS_VISIBLE, 20, 194, 420, 26, hwnd, nullptr,
                                          nullptr, nullptr);
            HWND save = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                        20, 240, 100, 30, hwnd, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                          130, 240, 100, 30, hwnd, (HMENU)(ID_BTN_SETTINGS + 1),
                                          nullptr, nullptr);
            for (HWND h : {g_cfg_key, g_cfg_url, g_cfg_model}) set_font(h, g_ui->f_ui);
            set_font(save, g_ui->f_ui);
            set_font(cancel, g_ui->f_ui);
            SetFocus(g_cfg_key);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, C_TEXT);
            SetBkColor(dc, C_FIELD);
            return (LRESULT)g_ui->b_field;
        }
        case WM_DRAWITEM: {
            auto* di = (LPDRAWITEMSTRUCT)lp;
            draw_button(di);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ID_BTN_SETTINGS) {
                std::string key = box_text(g_cfg_key);
                std::string url = box_text(g_cfg_url);
                std::string model = box_text(g_cfg_model);
                if (!key.empty()) g_cfg.api_key = key;
                if (!url.empty()) g_cfg.base_url = url;
                if (!model.empty()) g_cfg.model = model;
                // persist to hawk.ini next to the executable (never logged)
                FILE* f = fopen((app_dir() + "\\hawk.ini").c_str(), "w");
                if (f) {
                    fprintf(f, "# Hawk OS settings - keep this file to yourself\n");
                    if (!g_cfg.api_key.empty()) fprintf(f, "api_key=%s\n", g_cfg.api_key.c_str());
                    fprintf(f, "base_url=%s\n", g_cfg.base_url.c_str());
                    fprintf(f, "model=%s\n", g_cfg.model.c_str());
                    fprintf(f, "vision=%d\n", g_cfg.vision ? 1 : 0);
                    fclose(f);
                }
                g_cfg_saved = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == ID_BTN_SETTINGS + 1) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORBTN:
            return (LRESULT)g_ui->b_panel;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(C_PANEL);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, C_TEXT);
            HGDIOBJ old = SelectObject(dc, g_ui->f_bold);
            TextOutW(dc, 20, 18, L"Model settings", 14);
            SelectObject(dc, g_ui->f_ui);
            SetTextColor(dc, C_MUTED);
            TextOutW(dc, 20, 40, L"API key (NVIDIA, OpenAI, OpenRouter, local - stored in hawk.ini)", 69);
            TextOutW(dc, 20, 108, L"Base URL (OpenAI-compatible, .../v1)", 36);
            TextOutW(dc, 20, 174, L"Model id (use a vision model to let it see the screen)", 55);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_cfg_wnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void open_settings(HWND owner) {
    if (g_cfg_wnd) {
        SetForegroundWindow(g_cfg_wnd);
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = cfg_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HawkSettings";
    RegisterClassExW(&wc);

    RECT orc{};
    GetWindowRect(owner, &orc);
    g_cfg_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"HawkSettings",
                                L"Hawk OS - model settings",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                orc.left + 160, orc.top + 120, 470, 320, owner, nullptr,
                                wc.hInstance, nullptr);
    if (!g_cfg_wnd) return;
    SetForegroundWindow(g_cfg_wnd);
}

// ------------------------------------------------------------------ window --
static void run_goal_now() {
    std::string goal = trim(box_text(g_ui->goal));
    if (goal.empty()) {
        log_append(LogLine{"warn", "type a goal first, e.g. \"open notepad and write hello\""});
        return;
    }
    log_append(LogLine{"info", "goal: " + goal});
    start_job("goal", goal);
}

static void run_script_now() {
    std::string script = box_text(g_ui->script);
    if (trim(script).empty()) {
        log_append(LogLine{"warn", "the script box is empty"});
        return;
    }
    log_append(LogLine{"info", "script:\n" + script});
    start_job("script", script);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_ui->hwnd = hwnd;
            g_ui->f_ui = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                     OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
            g_ui->f_bold = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
            g_ui->f_title = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Segoe UI");
            g_ui->f_mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Consolas");
            g_ui->b_bg = CreateSolidBrush(C_BG);
            g_ui->b_panel = CreateSolidBrush(C_PANEL);
            g_ui->b_field = CreateSolidBrush(C_FIELD);
            g_ui->b_log = CreateSolidBrush(RGB(15, 15, 18));

            g_ui->goal = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         0, 0, 10, 10, hwnd, (HMENU)ID_GOAL, nullptr, nullptr);
            g_ui->script = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                                               WS_VSCROLL | ES_WANTRETURN,
                                           0, 0, 10, 10, hwnd, (HMENU)ID_SCRIPT, nullptr, nullptr);
            g_ui->log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                        0, 0, 10, 10, hwnd, (HMENU)ID_LOG, nullptr, nullptr);
            g_ui->btn_goal = make_button(hwnd, ID_BTN_GOAL, L"Run goal", g_ui->f_ui,
                                         L"Hand the goal and a screenshot to the model, then act");
            g_ui->btn_script = make_button(hwnd, ID_BTN_SCRIPT, L"Run script", g_ui->f_ui,
                                           L"Execute the commands above");
            g_ui->btn_stop = make_button(hwnd, ID_BTN_STOP, L"STOP", g_ui->f_bold,
                                         L"Abort the run (or hold ESC, or press Ctrl+Alt+H)");
            g_ui->btn_capture = make_button(hwnd, ID_BTN_CAPTURE, L"Capture", g_ui->f_ui,
                                            L"Save a screenshot to shots/ and refresh the view");
            g_ui->btn_folder = make_button(hwnd, ID_BTN_FOLDER, L"Shots folder", g_ui->f_ui, L"Open shots/");
            g_ui->btn_settings = make_button(hwnd, ID_BTN_SETTINGS, L"Model", g_ui->f_ui,
                                             L"API key, base URL and model id");
            g_ui->btn_clear = make_button(hwnd, ID_BTN_CLEAR, L"Clear", g_ui->f_ui, L"Clear the log");
            g_ui->quick1 = make_button(hwnd, ID_QUICK1, L"Click middle", g_ui->f_ui,
                                       L"Load a sample: click the centre of the screen");
            g_ui->quick2 = make_button(hwnd, ID_QUICK2, L"Notepad demo", g_ui->f_ui,
                                       L"Load a sample script that types into Notepad");
            g_ui->quick3 = make_button(hwnd, ID_QUICK3, L"Hello Harry", g_ui->f_ui,
                                       L"Load a sample: create a text file, write into it, read it "
                                       L"back and open it again");
            g_ui->quick4 = make_button(hwnd, ID_QUICK4, L"Help", g_ui->f_ui, L"Show the command list");

            g_ui->chk_dry = CreateWindowExW(0, L"BUTTON", L"Dry run",
                                            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10, hwnd,
                                            (HMENU)ID_CHK_DRYRUN, nullptr, nullptr);
            g_ui->chk_restore = CreateWindowExW(0, L"BUTTON", L"Restore cursor",
                                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                                hwnd, (HMENU)ID_CHK_RESTORE, nullptr, nullptr);
            g_ui->chk_click = CreateWindowExW(0, L"BUTTON", L"Click-through",
                                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                              hwnd, (HMENU)ID_CHK_CLICKTHRU, nullptr, nullptr);
            g_ui->chk_autorun = CreateWindowExW(0, L"BUTTON", L"Auto-run scripts",
                                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                                hwnd, (HMENU)ID_CHK_AUTORUN, nullptr, nullptr);
            SendMessageW(g_ui->chk_restore, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(g_ui->chk_autorun, BM_SETCHECK, BST_CHECKED, 0);

            for (HWND h : {g_ui->goal, g_ui->chk_dry, g_ui->chk_restore, g_ui->chk_click,
                           g_ui->chk_autorun})
                set_font(h, g_ui->f_ui);
            set_font(g_ui->script, g_ui->f_mono);
            set_font(g_ui->log, g_ui->f_mono);

            RegisterHotKey(hwnd, ID_HOTKEY_STOP, MOD_CONTROL | MOD_ALT, 'H');
            SetTimer(hwnd, 1, 160, nullptr);
            layout(hwnd);
            dump_layout(hwnd);
            set_running(false);  // STOP is grey until there is something to stop
            log_append(LogLine{"info", std::string(HAWK_NAME) + " " + HAWK_VERSION + " ready"});
            log_append(LogLine{"info", "Left: live screen. Right: the Agent. Try a quick action, or "
                                       "type a goal and press Run goal."});
            log_append(LogLine{"info", "Safety: hold ESC or press Ctrl+Alt+H to abort a run."});
            return 0;
        }
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // the double-buffered paint covers everything
        case WM_SIZE:
            layout(hwnd);
            dump_layout(hwnd);
            return 0;
        case WM_TIMER:
            compute_preview_rect(hwnd);
            g_ui->preview = capture_screen();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_LOG: {
            auto* line = (LogLine*)lp;
            if (line) {
                log_append(*line);
                delete line;
            }
            return 0;
        }
        case WM_APP_SCRIPT: {
            auto* script = (std::string*)lp;
            if (script) {
                SetWindowTextW(g_ui->script, wide(*script).c_str());
                delete script;
            }
            return 0;
        }
        case WM_APP_DONE:
            if (g_worker.joinable()) g_worker.join();
            set_running(false);
            return 0;
        case WM_HOTKEY:
            if (wp == ID_HOTKEY_STOP) {
                request_abort(true);
                log_append(LogLine{"warn", "STOP hotkey (Ctrl+Alt+H) pressed"});
                set_status("stopping...");
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            HWND ctl = (HWND)lp;
            SetTextColor(dc, C_TEXT);
            if (ctl == g_ui->script) {
                SetBkColor(dc, C_FIELD);
                return (LRESULT)g_ui->b_field;
            }
            SetBkColor(dc, RGB(15, 15, 18));
            return (LRESULT)g_ui->b_log;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, C_TEXT);
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)g_ui->b_panel;
        }
        case WM_DRAWITEM: {
            draw_button((LPDRAWITEMSTRUCT)lp);
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            // track hover for the owner-drawn buttons
            HWND over = ChildWindowFromPoint(hwnd, POINT{LOWORD(lp), HIWORD(lp)});
            if (over != g_hover) {
                g_hover = over;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hover = nullptr;
            return 0;
        case WM_SETCURSOR:
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_LBUTTONDOWN: {
            // clicking the preview can drive the real screen (opt in)
            if (!g_ui->click_through) break;
            if (GetTickCount() < g_ui->suppress_click_until) break;  // our own injected click
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (g_ui->preview_rect.contains(mx, my)) {
                Rect v = virtual_desktop();
                int sx = v.x + (int)((mx - g_ui->preview_rect.x) / g_ui->preview_scale);
                int sy = v.y + (int)((my - g_ui->preview_rect.y) / g_ui->preview_scale);
                std::string err;
                g_ui->suppress_click_until = GetTickCount() + 500;
                if (move_cursor(sx, sy, &err) && click_mouse("left", 1, &err)) {
                    g_ui->last_click = Point{sx, sy};
                    log_append(LogLine{"action", "click-through click at " + std::to_string(sx) + "," +
                                                      std::to_string(sy)});
                } else {
                    log_append(LogLine{"fail", "click-through: " + err});
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            switch (id) {
                case ID_BTN_GOAL: run_goal_now(); return 0;
                case ID_BTN_SCRIPT: run_script_now(); return 0;
                case ID_BTN_STOP:
                    request_abort(true);
                    log_append(LogLine{"warn", "STOP pressed"});
                    set_status("stopping...");
                    return 0;
                case ID_BTN_CAPTURE: {
                    std::string path = take_shot("app");
                    if (!path.empty()) {
                        g_ui->last_shot = path;
                        log_append(LogLine{"ok", "saved " + path});
                    }
                    g_ui->preview = capture_screen();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                case ID_BTN_FOLDER: {
                    std::string dir = shot_dir();
                    ShellExecuteW(nullptr, L"open", wide(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case ID_BTN_SETTINGS: open_settings(hwnd); return 0;
                case ID_BTN_CLEAR: SetWindowTextW(g_ui->log, L""); return 0;
                case ID_CHK_DRYRUN:
                    g_ui->dry_run = SendMessageW(g_ui->chk_dry, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    log_append(LogLine{"info", g_ui->dry_run ? "dry run on: nothing will be touched"
                                                             : "dry run off: actions are real"});
                    return 0;
                case ID_CHK_CLICKTHRU:
                    g_ui->click_through =
                        SendMessageW(g_ui->chk_click, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    return 0;
                case ID_CHK_AUTORUN:
                    g_ui->auto_run =
                        SendMessageW(g_ui->chk_autorun, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    log_append(LogLine{"info", g_ui->auto_run
                                                    ? "auto-run on: generated scripts are executed"
                                                    : "auto-run off: scripts are only generated"});
                    return 0;
                case ID_QUICK1: {
                    Rect v = virtual_desktop();
                    char buf[128];
                    snprintf(buf, sizeof(buf), "click left %d %d\n", v.x + v.w / 2, v.y + v.h / 2);
                    SetWindowTextW(g_ui->script, wide(buf).c_str());
                    return 0;
                }
                case ID_QUICK2: {
                    const wchar_t* demo =
                        L"open notepad\n"
                        L"wait 900\n"
                        L"focus notepad\n"
                        L"type Hawk OS typed this with real keystrokes.\n"
                        L"type Line two, and a save:\n"
                        L"key ctrl+s\n"
                        L"wait 500\n"
                        L"type hawk-demo.txt\n"
                        L"key enter\n"
                        L"shot after-notepad.png\n";
                    SetWindowTextW(g_ui->script, demo);
                    return 0;
                }
                case ID_QUICK3: {
                    // Create a text file, write into it, save it, read it back
                    // and open it again in whatever handles .txt on this PC.
                    const wchar_t* harry =
                        L"# Create a text file, write 'hello harry' into it, save it,\n"
                        L"# read it back and then open it again.\n"
                        L"write hello-harry.txt hello harry\n"
                        L"read hello-harry.txt\n"
                        L"open hello-harry.txt\n"
                        L"wait 2200\n"
                        L"focus hello-harry.txt\n"
                        L"shot hello-harry-opened.png\n"
                        L"windows 5\n";
                    SetWindowTextW(g_ui->script, harry);
                    return 0;
                }
                case ID_QUICK4: {
                    // Commented out so that pressing Run on the help text is
                    // harmless rather than a script of nonsense commands.
                    const wchar_t* help =
                        L"# Hawk OS commands - every one of these acts on the real machine\n"
                        L"#\n"
                        L"# shot [name.png]                      capture the whole screen\n"
                        L"# shotwin <title> [name.png]            capture one window\n"
                        L"# region x y w h                        part of the screen\n"
                        L"# move x y | move rel dx dy              move the real cursor\n"
                        L"# click [left|right|double] [x y]        real mouse click\n"
                        L"# drag x1 y1 x2 y2                       press, drag, release\n"
                        L"# scroll <notches>                       wheel (negative = down)\n"
                        L"# type <text>                           real keystrokes, \\n = Enter\n"
                        L"# key ctrl+s | key alt+f4 | key enter    key combinations\n"
                        L"# focus <title part>                    raise a window\n"
                        L"# open notepad | open https://...        launch something\n"
                        L"# close <title part>                    ask a window to close\n"
                        L"# wait <ms>                             pause\n"
                        L"# pixel x y                             colour at a point\n"
                        L"# find RRGGBB [tol] [move|click|double] find a colour, act on it\n"
                        L"# verifypixel x y RRGGBB [tol]          assert what is on screen\n"
                        L"# windows | monitors | cursor            describe the machine\n"
                        L"# write <path> <text> | mkdir <path>    create files/folders\n"
                        L"# try <cmd>                             ignore a failure\n"
                        L"#\n"
                        L"# Delete these comment lines and write your own commands here.\n";
                    SetWindowTextW(g_ui->script, help);
                    return 0;
                }
            }
            break;
        }
        case WM_CLOSE: {
            request_abort(true);
            if (g_worker.joinable()) g_worker.join();
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            UnregisterHotKey(hwnd, ID_HOTKEY_STOP);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int run_gui(HINSTANCE inst, Config cfg) {
    enable_dpi_awareness();
    g_cfg = cfg;
    UiState state;
    g_ui = &state;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HawkOSMain";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    Rect v = virtual_desktop();
    int w = v.w > 1400 ? 1400 : (v.w - 60);
    int h = v.h > 900 ? 900 : (v.h - 80);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HWND hwnd = CreateWindowExW(0, L"HawkOSMain", L"Hawk OS - desktop control agent",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, v.x + 40, v.y + 40, w, h,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    // Come to the front: the user just launched this, it should not open behind
    // whatever was focused before.
    if (!focus_window(hwnd)) focus_window(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_worker.joinable()) g_worker.join();
    return 0;
}

}  // namespace hawk
