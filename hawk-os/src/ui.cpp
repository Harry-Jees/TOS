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

#ifndef ES_DISABLENOSCROLL  // missing from the bundled headers; it is 0x00020000
#define ES_DISABLENOSCROLL 0x00020000L
#endif

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
    ID_LBL_GOAL,
    ID_LBL_SCRIPT,
    ID_LBL_LOG,
    ID_HOTKEY_STOP = 1,
};

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_SCRIPT (WM_APP + 3)

static const int HEADER_H = 62;
#define HEADER_TOP() (HEADER_H + 10)

// Obsidian + warm metallic amber: clear contrast, restrained accents, less generic neon.
static const COLORREF C_BG = RGB(11, 12, 15);
static const COLORREF C_PANEL = RGB(22, 23, 27);
static const COLORREF C_HEADER1 = RGB(18, 43, 47);
static const COLORREF C_HEADER2 = RGB(9, 13, 18);
static const COLORREF C_TEXT = RGB(248, 246, 240);
static const COLORREF C_MUTED = RGB(171, 166, 155);
static const COLORREF C_ACCENT = RGB(83, 218, 204);
static const COLORREF C_DANGER = RGB(235, 83, 75);
static const COLORREF C_BTN = RGB(25, 34, 39);
static const COLORREF C_BTN_HI = RGB(35, 53, 59);
static const COLORREF C_LINE = RGB(42, 64, 70);
static const COLORREF C_FIELD = RGB(12, 20, 24);
static const COLORREF C_OK = RGB(112, 218, 158);
static const COLORREF C_WARN = RGB(245, 190, 89);
static const COLORREF C_OFF = RGB(132, 128, 120);

struct UiState {
    HWND hwnd = nullptr;
    HWND goal = nullptr, script = nullptr, log = nullptr, status = nullptr;
    HWND lbl_goal = nullptr, lbl_script = nullptr, lbl_log = nullptr;
    HWND btn_goal = nullptr, btn_script = nullptr, btn_stop = nullptr, btn_capture = nullptr;
    HWND btn_folder = nullptr, btn_settings = nullptr, btn_clear = nullptr;
    HWND chk_dry = nullptr, chk_restore = nullptr, chk_autorun = nullptr;
    HWND quick1 = nullptr, quick2 = nullptr, quick3 = nullptr, quick4 = nullptr;

    HFONT f_ui = nullptr, f_bold = nullptr, f_title = nullptr, f_mono = nullptr, f_label = nullptr, f_icon = nullptr;
    HBRUSH b_bg = nullptr, b_panel = nullptr, b_field = nullptr, b_log = nullptr;
    bool dry_run = false;
    bool auto_run = false;
    bool follow_log = true;
    std::string status_text = "idle";
    std::string run_state = "idle";   // idle | running | stopping
    std::string run_kind;             // goal | script (what is running)
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
    wchar_t stamp[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    // `12:04:31 [ok   ] text` - the level tag mirrors the file log, so the
    // scrollback stays readable after a long run scrolls past.
    swprintf(stamp, 64, L"%02d:%02d:%02d [%-6s] ", st.wHour, st.wMinute, st.wSecond,
             wide(line.level).c_str());
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

static void set_running(bool on, const char* kind = nullptr) {
    g_running = on;
    if (!g_ui) return;
    if (on && kind) g_ui->run_kind = kind;
    if (!on) g_ui->run_kind.clear();
    g_ui->run_state = on ? "running" : "idle";
    EnableWindow(g_ui->btn_goal, !on);
    EnableWindow(g_ui->btn_script, !on);
    EnableWindow(g_ui->quick1, !on);
    EnableWindow(g_ui->quick2, !on);
    EnableWindow(g_ui->quick3, !on);
    EnableWindow(g_ui->btn_stop, on);
    set_status(on ? (g_ui->run_kind == "goal"
                         ? "planning from the screen - STOP or hold ESC to abort"
                         : "running the script - STOP or hold ESC to abort")
                  : "idle");
    if (g_ui->hwnd) InvalidateRect(g_ui->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------- worker thread ---
static void start_job(const std::string& kind, const std::string& payload) {
    if (g_running) {
        log_append(LogLine{"warn", "a run is already in progress"});
        return;
    }
    bool screen_upload_approved = false;
    if (kind == "goal" && g_cfg.vision && !g_cfg.api_key.empty()) {
        int consent = MessageBoxW(
            g_ui->hwnd,
            L"Hawk will capture your screen and send the screenshot to the model provider configured in Model settings. Screenshots may contain private information. Continue only if you trust that provider.",
            L"Screen privacy check", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (consent != IDOK) {
            log_append(LogLine{"info", "goal cancelled before sending a screenshot"});
            return;
        }
        screen_upload_approved = true;
    }
    if (g_worker.joinable()) g_worker.join();

    Config cfg;
    cfg.dry_run = g_ui->dry_run;
    cfg.restore_cursor = SendMessageW(g_ui->chk_restore, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.api_key = g_cfg.api_key;
    cfg.allow_screen_upload = screen_upload_approved;
    cfg.base_url = g_cfg.base_url;
    cfg.model = g_cfg.model;
    cfg.vision = g_cfg.vision;
    cfg.step_delay_ms = g_cfg.step_delay_ms;
    cfg.auto_run = g_ui->auto_run;

    request_abort(false);
    set_running(true, kind.c_str());
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
        agent.on_approval = [](const std::string& action, const std::string& details) {
            std::wstring prompt = L"Hawk is requesting permission for a sensitive action.\n\n";
            prompt += L"Action: " + wide(action) + L"\nCommand: " + wide(details);
            prompt += L"\n\nAllow this step only?";
            return MessageBoxW(g_ui->hwnd, prompt.c_str(), L"Hawk OS - approval required",
                               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        };
        if (kind == "goal") agent.run_goal(payload);
        else agent.run_script(payload);
        PostMessageW(g_ui->hwnd, WM_APP_DONE, 0, 0);
    });
}

// ------------------------------------------------------------- preview ------
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
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    // Refreshed control-center canvas: no embedded screenshot; Hawk captures only
    // when a task needs vision or the user presses Capture.
    RECT top{0, 0, rc.right, 94};
    TRIVERTEX verts[2] = {
        {0, 0, (COLOR16)(GetRValue(C_HEADER1) << 8), (COLOR16)(GetGValue(C_HEADER1) << 8),
         (COLOR16)(GetBValue(C_HEADER1) << 8), 0xFF00},
        {rc.right, 94, (COLOR16)(GetRValue(C_BG) << 8), (COLOR16)(GetGValue(C_BG) << 8),
         (COLOR16)(GetBValue(C_BG) << 8), 0xFF00}};
    GRADIENT_RECT gr{0, 1};
    if (!GradientFill(mem, verts, 2, &gr, 1, GRADIENT_FILL_RECT_V)) {
        HBRUSH hb = CreateSolidBrush(C_HEADER1);
        FillRect(mem, &top, hb);
        DeleteObject(hb);
    }

    SelectObject(mem, g_ui->f_title);
    SetTextColor(mem, C_TEXT);
    TextOutW(mem, 26, 18, L"HAWK OS", 7);
    SelectObject(mem, g_ui->f_ui);
    SetTextColor(mem, C_MUTED);
    const wchar_t* tagline = L"DESKTOP AUTOMATION  /  PRIVATE BY DEFAULT  /  NVIDIA NIM";
    TextOutW(mem, 28, 52, tagline, (int)wcslen(tagline));

    const bool active = g_ui->run_state == "running" || g_ui->run_state == "stopping";
    COLORREF state_color = active ? C_OK : (g_cfg.api_key.empty() ? C_WARN : C_ACCENT);
    const wchar_t* state = g_ui->run_state == "stopping" ? L"STOPPING" :
                           g_ui->run_state == "running" ? L"WORKING" :
                           g_cfg.api_key.empty() ? L"SET UP NVIDIA KEY" : L"READY";
    RECT status{rc.right - 210, 25, rc.right - 26, 59};
    draw_round(mem, status, 16, C_PANEL, state_color, true);
    if (active) {
        const int pulse = (int)((GetTickCount() / 110) % 3);
        for (int i = 0; i < 3; ++i) {
            RECT dot{status.left + 14 + i * 8, status.top + 14,
                     status.left + 19 + i * 8, status.top + 19};
            COLORREF col = i == pulse ? state_color : C_OFF;
            HBRUSH db = CreateSolidBrush(col);
            FillRect(mem, &dot, db);
            DeleteObject(db);
        }
    } else {
        RECT dot{status.left + 15, status.top + 14, status.left + 22, status.top + 21};
        HBRUSH db = CreateSolidBrush(state_color);
        HBRUSH ob = (HBRUSH)SelectObject(mem, db);
        Ellipse(mem, dot.left, dot.top, dot.right, dot.bottom);
        SelectObject(mem, ob);
        DeleteObject(db);
    }
    SelectObject(mem, g_ui->f_bold);
    SetTextColor(mem, state_color);
    RECT sr{status.left + 31, status.top, status.right - 8, status.bottom};
    DrawTextW(mem, state, -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int margin = 24;
    int card_w = (rc.right - margin * 2 - 14) / 2;
    int top_y = 112;
    RECT goal_card{margin, top_y, margin + card_w, top_y + 154};
    RECT quick_card{margin + card_w + 14, top_y, rc.right - margin, top_y + 154};
    draw_round(mem, goal_card, 16, C_PANEL, C_LINE, true);
    draw_round(mem, quick_card, 16, C_PANEL, C_LINE, true);

    SelectObject(mem, g_ui->f_bold);
    SetTextColor(mem, C_TEXT);
    TextOutW(mem, goal_card.left + 18, goal_card.top + 15, L"01   TASK BRIEF", 15);
    SelectObject(mem, g_ui->f_ui);
    SetTextColor(mem, C_MUTED);

    SelectObject(mem, g_ui->f_bold);
    SetTextColor(mem, C_TEXT);
    TextOutW(mem, quick_card.left + 18, quick_card.top + 15, L"02   QUICK START", 15);
    SelectObject(mem, g_ui->f_ui);
    SetTextColor(mem, C_MUTED);
    const wchar_t* quick_hint = L"Start with a safe demo, load a script, or capture a screenshot to file.";
    RECT qh{quick_card.left + 18, quick_card.top + 42, quick_card.right - 16, quick_card.top + 76};
    DrawTextW(mem, quick_hint, -1, &qh, DT_LEFT | DT_WORDBREAK);
    SetTextColor(mem, C_ACCENT);
    const wchar_t* safe_hint = L"DRY RUN is recommended for your first task";
    TextOutW(mem, quick_card.left + 18, quick_card.top + 91, safe_hint, (int)wcslen(safe_hint));

    RECT work{margin, top_y + 140, rc.right - margin, rc.bottom - 18};
    draw_round(mem, work, 16, C_PANEL, C_LINE, true);
    SelectObject(mem, g_ui->f_bold);
    SetTextColor(mem, C_TEXT);
    TextOutW(mem, work.left + 18, work.top + 14, L"WORKSPACE", 9);
    SelectObject(mem, g_ui->f_ui);
    SetTextColor(mem, C_MUTED);
    const wchar_t* workspace_hint = L"Edit the generated script, review activity, and control execution below.";
    TextOutW(mem, work.left + 18, work.top + 38, workspace_hint, (int)wcslen(workspace_hint));

    if (g_ui->dry_run) {
        RECT dry{rc.right - 144, work.top + 12, rc.right - 28, work.top + 38};
        draw_round(mem, dry, 12, RGB(53, 39, 17), C_WARN, true);
        SelectObject(mem, g_ui->f_bold);
        SetTextColor(mem, C_WARN);
        DrawTextW(mem, L"DRY RUN ON", -1, &dry, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (!g_ui->last_shot.empty()) {
        std::wstring shot = L"Last capture saved: " + wide(g_ui->last_shot);
        SelectObject(mem, g_ui->f_ui);
        SetTextColor(mem, C_MUTED);
        RECT r{work.left + 18, work.bottom - 27, work.right - 18, work.bottom - 8};
        DrawTextW(mem, shot.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- layout ----
static void layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int margin = 24;
    const int gap = 12;
    const int width = rc.right - margin * 2;
    const int half = (width - gap) / 2;
    int y = 154;

    MoveWindow(g_ui->lbl_goal, margin + 18, y, half - 36, 16, TRUE);
    y += 20;
    MoveWindow(g_ui->goal, margin + 18, y, half - 36, 32, TRUE);
    MoveWindow(g_ui->btn_goal, margin + 18, y + 42, 112, 34, TRUE);
    MoveWindow(g_ui->btn_capture, margin + 138, y + 42, 102, 34, TRUE);
    MoveWindow(g_ui->btn_stop, margin + 248, y + 42, 82, 34, TRUE);
    MoveWindow(g_ui->btn_settings, margin + 338, y + 42, 94, 34, TRUE);

    int qx = margin + half + gap + 18;
    MoveWindow(g_ui->quick1, qx, 195, 96, 30, TRUE);
    MoveWindow(g_ui->quick2, qx + 104, 195, 104, 30, TRUE);
    MoveWindow(g_ui->quick3, qx + 216, 195, 104, 30, TRUE);
    MoveWindow(g_ui->quick4, qx + 328, 195, 80, 30, TRUE);

    MoveWindow(g_ui->chk_dry, margin + 18, 248, 112, 22, TRUE);
    MoveWindow(g_ui->chk_restore, margin + 138, 248, 142, 22, TRUE);
    MoveWindow(g_ui->chk_autorun, margin + 292, 248, 132, 22, TRUE);

    const int panel_top = 280;
    const int panel_bottom = rc.bottom - 18;
    const int col_gap = 14;
    const int col_w = (width - col_gap) / 2;
    int col_x = margin;
    int label_y = panel_top + 64;
    int script_y = label_y + 20;
    int button_y = panel_bottom - 42;
    int field_h = button_y - script_y - 40;
    if (field_h < 70) field_h = 70;
    MoveWindow(g_ui->lbl_script, col_x + 18, label_y, col_w - 36, 16, TRUE);
    MoveWindow(g_ui->script, col_x + 18, script_y, col_w - 36, field_h, TRUE);
    MoveWindow(g_ui->btn_script, col_x + 18, button_y, 104, 30, TRUE);
    MoveWindow(g_ui->btn_clear, col_x + 130, button_y, 74, 30, TRUE);
    MoveWindow(g_ui->btn_folder, col_x + col_w - 120, button_y, 102, 30, TRUE);

    int log_x = margin + col_w + col_gap;
    MoveWindow(g_ui->lbl_log, log_x + 18, label_y, col_w - 36, 16, TRUE);
    MoveWindow(g_ui->log, log_x + 18, script_y, col_w - 36, field_h, TRUE);

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
        {"dry_run", g_ui->chk_dry},
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
    if (is_goal) fill = pressed ? RGB(0, 135, 185) : C_ACCENT;
    else if (is_stop) fill = pressed ? RGB(170, 40, 34) : C_DANGER;
    else fill = pressed ? RGB(44, 44, 52) : (hot ? C_BTN_HI : C_BTN);
    if (disabled) fill = RGB(42, 42, 48);

    RECT r = di->rcItem;
    // The accent and STOP fills never change hue on hover (that would make the
    // button look like a different button); they gain a bright edge instead.
    if (is_goal)
        draw_round(di->hDC, r, 8, fill, RGB(170, 210, 255), hot && !disabled);
    else if (is_stop)
        draw_round(di->hDC, r, 8, fill, RGB(255, 190, 185), hot && !disabled);
    else
        draw_round(di->hDC, r, 8, fill, C_LINE, true);

    wchar_t text[64] = {0};
    GetWindowTextW(di->hwndItem, text, 63);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? C_MUTED : C_TEXT);
    wchar_t glyph = 0;
    switch (di->CtlID) {
        case ID_BTN_GOAL: glyph = 0xE768; break;
        case ID_BTN_SCRIPT: glyph = 0xE943; break;
        case ID_BTN_STOP: glyph = 0xE71A; break;
        case ID_BTN_CAPTURE: glyph = 0xE722; break;
        case ID_BTN_FOLDER: glyph = 0xE8B7; break;
        case ID_BTN_SETTINGS: glyph = 0xE713; break;
        case ID_BTN_CLEAR: glyph = 0xE74D; break;
        case ID_QUICK1: glyph = 0xE8A7; break;
        case ID_QUICK2: glyph = 0xE8A5; break;
        case ID_QUICK3: glyph = 0xE8D4; break;
        case ID_QUICK4: glyph = 0xE897; break;
    }
    RECT text_rc = r;
    if (glyph && g_ui->f_icon) {
        RECT ir{r.left + 8, r.top, r.left + 27, r.bottom};
        wchar_t icon_text[2] = {glyph, 0};
        HGDIOBJ icon_font = SelectObject(di->hDC, g_ui->f_icon);
        DrawTextW(di->hDC, icon_text, -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(di->hDC, icon_font);
        text_rc.left += 12;
    }
    HGDIOBJ oldf = SelectObject(di->hDC, g_ui->f_ui);
    DrawTextW(di->hDC, text, -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(di->hDC, oldf);
}

// --------------------------------------------------------------- settings ---
static HWND g_cfg_wnd = nullptr, g_cfg_key = nullptr, g_cfg_url = nullptr, g_cfg_model = nullptr;
static bool g_cfg_saved = false;

static LRESULT CALLBACK cfg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_cfg_key = CreateWindowExW(0, L"STATIC", L"Credential source: .env beside hawk.exe (NVIDIA_API_KEY)",
                                        WS_CHILD | WS_VISIBLE, 24, 62, 500, 28, hwnd, nullptr, nullptr, nullptr);
            g_cfg_url = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wide(g_cfg.base_url).c_str(),
                                        WS_CHILD | WS_VISIBLE, 24, 128, 500, 28, hwnd, nullptr,
                                        nullptr, nullptr);
            g_cfg_model = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wide(g_cfg.model).c_str(),
                                          WS_CHILD | WS_VISIBLE, 24, 194, 500, 28, hwnd, nullptr,
                                          nullptr, nullptr);
            HWND save = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                        24, 286, 100, 32, hwnd, (HMENU)ID_BTN_SETTINGS, nullptr, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                          136, 286, 100, 32, hwnd, (HMENU)(ID_BTN_SETTINGS + 1),
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
                std::string url = box_text(g_cfg_url);
                std::string model = box_text(g_cfg_model);
                if (!url.empty()) g_cfg.base_url = url;
                if (!model.empty()) g_cfg.model = model;
                // Never save or replace the credential from this dialog: startup
                // credentials must come only from the user's local .env file.
                FILE* f = fopen((app_dir() + "\\hawk.ini").c_str(), "w");
                if (f) {
                    fprintf(f, "# Non-secret Hawk OS preferences; API credentials belong in .env\n");
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
            TextOutW(dc, 24, 18, L"MODEL CONNECTION", 16);
            SelectObject(dc, g_ui->f_ui);
            SetTextColor(dc, C_MUTED);
            const wchar_t* l1 = L"API key is read only from the local .env file; this dialog never stores it.";
            const wchar_t* l2 = L"NVIDIA NIM API base URL";
            const wchar_t* l3 = L"Vision-capable model ID (default uses NVIDIA Nemotron vision)";
            TextOutW(dc, 24, 40, l1, (int)wcslen(l1));
            TextOutW(dc, 24, 106, l2, (int)wcslen(l2));
            TextOutW(dc, 24, 172, l3, (int)wcslen(l3));
            SetTextColor(dc, C_ACCENT);
            const wchar_t* l4 = L"Keep .env private; it is ignored by Git. Restart Hawk after changing the key.";
            TextOutW(dc, 24, 238, l4, (int)wcslen(l4));
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
                                L"HAWK OS - model connection",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                orc.left + 160, orc.top + 120, 570, 370, owner, nullptr,
                                wc.hInstance, nullptr);
    if (!g_cfg_wnd) return;
    SetForegroundWindow(g_cfg_wnd);
}

// ------------------------------------------------------------------ window --
static void run_goal_now() {
    std::string goal = trim(box_text(g_ui->goal));
    if (goal.empty()) {
        log_append(LogLine{"warn", "Enter a goal first, for example: open Notepad and type hello."});
        return;
    }
    if (g_cfg.api_key.empty()) {
        log_append(LogLine{"fail", "NVIDIA_API_KEY is missing. Add it to .env beside hawk.exe, then restart Hawk."});
        MessageBoxW(g_ui->hwnd,
                    L"Hawk cannot call NVIDIA NIM because NVIDIA_API_KEY is missing.\n\nCreate .env beside hawk.exe and add NVIDIA_API_KEY=your_key, then restart Hawk.",
                    L"Hawk OS - NVIDIA setup required", MB_OK | MB_ICONWARNING);
        open_settings(g_ui->hwnd);
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
            g_ui->f_label = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Segoe UI");
            g_ui->f_icon = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe MDL2 Assets");
            g_ui->b_bg = CreateSolidBrush(C_BG);
            g_ui->b_panel = CreateSolidBrush(C_PANEL);
            g_ui->b_field = CreateSolidBrush(C_FIELD);
            g_ui->b_log = CreateSolidBrush(RGB(15, 15, 18));

            g_ui->goal = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         0, 0, 10, 10, hwnd, (HMENU)ID_GOAL, nullptr, nullptr);
            // ES_DISABLENOSCROLL keeps the text column from jumping sideways
            // when a run makes the log or script grow past the window.
            g_ui->script = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                           WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                                               WS_VSCROLL | ES_WANTRETURN | ES_DISABLENOSCROLL,
                                           0, 0, 10, 10, hwnd, (HMENU)ID_SCRIPT, nullptr, nullptr);
            g_ui->log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                            WS_VSCROLL | ES_DISABLENOSCROLL,
                                        0, 0, 10, 10, hwnd, (HMENU)ID_LOG, nullptr, nullptr);
            g_ui->lbl_goal = CreateWindowExW(0, L"STATIC", L"GOAL - tell the agent what to achieve",
                                             WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                                             (HMENU)ID_LBL_GOAL, nullptr, nullptr);
            g_ui->lbl_script = CreateWindowExW(0, L"STATIC", L"SCRIPT - commands run on the real machine",
                                               WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                                               (HMENU)ID_LBL_SCRIPT, nullptr, nullptr);
            g_ui->lbl_log = CreateWindowExW(0, L"STATIC", L"ACTIVITY",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd,
                                            (HMENU)ID_LBL_LOG, nullptr, nullptr);
            g_ui->btn_goal = make_button(hwnd, ID_BTN_GOAL, L"Run goal", g_ui->f_ui,
                                         L"Hand the goal and a screenshot to the model, then act");
            g_ui->btn_script = make_button(hwnd, ID_BTN_SCRIPT, L"Run script", g_ui->f_ui,
                                           L"Execute the commands above");
            g_ui->btn_stop = make_button(hwnd, ID_BTN_STOP, L"STOP", g_ui->f_bold,
                                         L"Abort the run (or hold ESC, or press Ctrl+Alt+H)");
            g_ui->btn_capture = make_button(hwnd, ID_BTN_CAPTURE, L"Capture", g_ui->f_ui,
                                            L"Capture the desktop to a file; the image is not displayed in Hawk");
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
            g_ui->chk_autorun = CreateWindowExW(0, L"BUTTON", L"Auto-run (risky)",
                                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                                hwnd, (HMENU)ID_CHK_AUTORUN, nullptr, nullptr);
            SendMessageW(g_ui->chk_restore, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(g_ui->chk_autorun, BM_SETCHECK, BST_UNCHECKED, 0);

            for (HWND h : {g_ui->goal, g_ui->chk_dry, g_ui->chk_restore,
                           g_ui->chk_autorun})
                set_font(h, g_ui->f_ui);
            for (HWND h : {g_ui->lbl_goal, g_ui->lbl_script, g_ui->lbl_log})
                set_font(h, g_ui->f_label);
            set_font(g_ui->script, g_ui->f_mono);
            set_font(g_ui->log, g_ui->f_mono);

            RegisterHotKey(hwnd, ID_HOTKEY_STOP, MOD_CONTROL | MOD_ALT, 'H');
            SetTimer(hwnd, 1, 120, nullptr);
            layout(hwnd);
            dump_layout(hwnd);
            set_running(false);  // STOP is grey until there is something to stop
            log_append(LogLine{"info", std::string(HAWK_NAME) + " " + HAWK_VERSION + " ready"});
            log_append(LogLine{"info", "Control center ready. Enter a goal, review scripts, or choose a quick action."});
            log_append(LogLine{"info", "Screenshots are captured only when a task needs vision or you press Capture; they are never displayed in this window."});
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
            if (g_ui->run_state == "running" || g_ui->run_state == "stopping")
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
                g_ui->run_state = "stopping";
                log_append(LogLine{"warn", "STOP hotkey (Ctrl+Alt+H) pressed"});
                set_status("stopping...");
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            HWND ctl = (HWND)lp;
            if (ctl == g_ui->log) {
                // The log is a running record: kept calm and muted so the
                // newest entry, not the loudest colour, is what you read.
                SetTextColor(dc, C_MUTED);
                SetBkColor(dc, RGB(15, 15, 18));
                return (LRESULT)g_ui->b_log;
            }
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
            HWND ctl = (HWND)lp;
            bool is_label = ctl == g_ui->lbl_goal || ctl == g_ui->lbl_script ||
                            ctl == g_ui->lbl_log;
            SetTextColor(dc, is_label ? C_MUTED : C_TEXT);
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
        case WM_COMMAND: {
            int id = LOWORD(wp);
            switch (id) {
                case ID_BTN_GOAL: run_goal_now(); return 0;
                case ID_BTN_SCRIPT: run_script_now(); return 0;
                case ID_BTN_STOP:
                    request_abort(true);
                    g_ui->run_state = "stopping";
                    log_append(LogLine{"warn", "STOP pressed"});
                    set_status("stopping...");
                    if (g_ui->hwnd) InvalidateRect(g_ui->hwnd, nullptr, FALSE);
                    return 0;
                case ID_BTN_CAPTURE: {
                    std::string path = take_shot("app");
                    if (!path.empty()) {
                        g_ui->last_shot = path;
                        log_append(LogLine{"ok", "saved " + path});
                    }
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
                    // the caption itself carries the state - no reading a tiny
                    // checkbox to know whether this run is real
                    SetWindowTextW(g_ui->chk_dry, g_ui->dry_run ? L"Dry run: ON" : L"Dry run");
                    log_append(LogLine{"info", g_ui->dry_run ? "dry run on: nothing will be touched"
                                                             : "dry run off: actions are real"});
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case ID_CHK_AUTORUN: {
                    bool enable =
                        SendMessageW(g_ui->chk_autorun, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (enable) {
                        int consent = MessageBoxW(
                            hwnd,
                            L"Auto-run allows model-generated commands to control your real desktop. A malicious webpage or on-screen prompt may try to manipulate the model. Only enable this for tasks you trust. Continue?",
                            L"Enable risky automation?", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
                        if (consent != IDOK) {
                            enable = false;
                            SendMessageW(g_ui->chk_autorun, BM_SETCHECK, BST_UNCHECKED, 0);
                        }
                    }
                    g_ui->auto_run = enable;
                    log_append(LogLine{"info", g_ui->auto_run
                                                    ? "auto-run enabled by user confirmation"
                                                    : "auto-run off: generated scripts are only reviewed"});
                    return 0;
                }
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
                    // A real editor round-trip: open Notepad, type the line
                    // with synthetic keystrokes, save through Notepad's own
                    // Save dialog, close it, then open the saved file again
                    // from disk and read it back to prove it persisted.
                    const wchar_t* harry =
                        L"# Create a text document, write 'hello harry', save it,\n"
                        L"# close it and open it again from disk.\n"
                        L"del ~\\Documents\\hello-harry.txt\n"
                        L"open notepad\n"
                        L"wait 2400\n"
                        L"focus Notepad\n"
                        L"type hello harry\n"
                        L"key ctrl+s\n"
                        L"wait 2000\n"
                        L"type hello-harry.txt\n"
                        L"key enter\n"
                        L"wait 2000\n"
                        L"close Notepad\n"
                        L"waitclose Notepad 8000\n"
                        L"wait 1500\n"
                        L"open ~\\Documents\\hello-harry.txt\n"
                        L"wait 2600\n"
                        L"focus Notepad\n"
                        L"read ~\\Documents\\hello-harry.txt\n"
                        L"shot hello-harry-opened.png\n"
                        L"windows 6\n"
                        L"log hello harry: created, written, saved and reopened\n";
                    SetWindowTextW(g_ui->script, harry);
                    return 0;
                }
                case ID_QUICK4: {
                    // Commented out so that pressing Run on the help text is
                    // harmless rather than a script of nonsense commands.
                    const wchar_t* help =
                        L"# Hawk OS commands - every one of these acts on the real machine\n"
                        L"#\n"
                        L"# --- seeing ---------------------------------------------------------\n"
                        L"# shot [name.png]                       capture the whole screen\n"
                        L"# shotwin <title> [name.png]            capture one window\n"
                        L"# region x y w h                        part of the screen\n"
                        L"# look                                 analyse the screen: list the\n"
                        L"#                                      controls it can detect\n"
                        L"# elements [x y w h]                    same, with clickable coordinates\n"
                        L"# pixel x y                             colour at a point\n"
                        L"# windows | monitors | cursor           describe the machine\n"
                        L"# --- touching -------------------------------------------------------\n"
                        L"# move x y | move rel dx dy             move the real cursor\n"
                        L"# click [left|right|double] [x y]       real mouse click\n"
                        L"# clickel <rank> [right|double]         click the Nth detected control\n"
                        L"# drag x1 y1 x2 y2                      press, drag, release\n"
                        L"# scroll <notches>                      wheel (negative = down)\n"
                        L"# smove x y | sclick [btn] x y | sdrag ..  aim at the last screenshot;\n"
                        L"#                                      Hawk maps it onto the real screen\n"
                        L"# type <text>                           real keystrokes,  \\n = Enter\n"
                        L"# key ctrl+s | key alt+f4 | key enter   key combinations\n"
                        L"# --- windows --------------------------------------------------------\n"
                        L"# focus <title part>                    raise a window\n"
                        L"# waitfocus <title part> [ms]           wait for a window, then focus it\n"
                        L"# open notepad | open https://...       launch something\n"
                        L"# close <title part>                    ask a window to close\n"
                        L"# waitclose <title part> [ms]           wait until a window is gone\n"
                        L"# wait <ms>                             pause\n"
                        L"# --- finding and verifying ------------------------------------------\n"
                        L"# find RRGGBB [tol] [move|click|double]  find a colour, act on it\n"
                        L"# findarea RRGGBB [tol] [move|click|double]  same, on whole regions\n"
                        L"# verifypixel x y RRGGBB [tol]          assert what is on screen\n"
                        L"# --- files and flow -------------------------------------------------\n"
                        L"# write <path> <text> | mkdir <path>    create files/folders\n"
                        L"# ensure <path> [text]                  create a file only if missing\n"
                        L"# read <path>                           print a file into the log\n"
                        L"# del <path>                            delete a file (~ = your folder)\n"
                        L"# try <cmd>                             ignore a failure\n"
                        L"# log <text> | done                     note; finish\n"
                        L"#\n"
                        L"# Delete these comment lines and write your own commands here.\n";
                    SetWindowTextW(g_ui->script, help);
                    return 0;
                }
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            // Keep the window big enough that the Agent panel never collapses,
            // and never larger than the desktop it lives on.
            auto* mmi = (MINMAXINFO*)lp;
            Rect v = virtual_desktop();
            mmi->ptMinTrackSize.x = v.w > 940 ? 880 : v.w - 40;
            mmi->ptMinTrackSize.y = v.h > 640 ? 580 : v.h - 60;
            if (mmi->ptMinTrackSize.x < 640) mmi->ptMinTrackSize.x = 640;
            if (mmi->ptMinTrackSize.y < 440) mmi->ptMinTrackSize.y = 440;
            return 0;
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
            for (HFONT* f : {&g_ui->f_ui, &g_ui->f_bold, &g_ui->f_title, &g_ui->f_mono,
                             &g_ui->f_label, &g_ui->f_icon})
                if (*f) {
                    DeleteObject(*f);
                    *f = nullptr;
                }
            for (HBRUSH* b : {&g_ui->b_bg, &g_ui->b_panel, &g_ui->b_field, &g_ui->b_log})
                if (*b) {
                    DeleteObject(*b);
                    *b = nullptr;
                }
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
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                 LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
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
