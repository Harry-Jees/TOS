// Hawk OS - entry point: GUI by default, CLI when given arguments, plus the
// self-test that proves capture and physical input actually work.
#include "hawk.h"

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hawk {

// ---------------------------------------------------------------- settings --
static void read_ini(Config* cfg) {
    std::string path = app_dir() + "\\hawk.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    std::string legacy_key, protected_key;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = to_lower(trim(s.substr(0, eq)));
        std::string v = trim(s.substr(eq + 1));
        if (k == "api_key_dpapi") {
            protected_key = v;
        } else if (k == "api_key") {
            // Legacy compatibility: old versions wrote plaintext keys. A
            // protected key always takes precedence if both settings exist.
            legacy_key = v;
        }
        else if (k == "base_url") cfg->base_url = v;
        else if (k == "model") cfg->model = v;
        else if (k == "vision") cfg->vision = (v != "0");
        else if (k == "dry_run") cfg->dry_run = (v == "1" || to_lower(v) == "true");
        else if (k == "step_delay_ms") { try { cfg->step_delay_ms = std::stoi(v); } catch (...) {} }
    }
    fclose(f);
    cfg->api_key = !protected_key.empty() ? unprotect_secret(protected_key) : legacy_key;
}

static const char* env_or_null(const char* name) {
    const char* v = getenv(name);
    return (v && *v) ? v : nullptr;
}

// Settings come from hawk.ini next to the executable, then environment
// variables, then command line flags. The key is never printed anywhere.
void load_settings(Config* cfg) {
    read_ini(cfg);
    if (const char* v = env_or_null("HAWK_API_KEY")) cfg->api_key = v;
    if (const char* v = env_or_null("HAWK_BASE_URL")) cfg->base_url = v;
    if (const char* v = env_or_null("HAWK_MODEL")) cfg->model = v;
}

static void usage() {
    printf("%s %s - desktop control agent\n\n", HAWK_NAME, HAWK_VERSION);
    printf("Run with no arguments to open the app window.\n\n");
    printf("  --run \"<script>\"      execute commands (see below)\n");
    printf("  --file <path>          execute a script file\n");
    printf("  --goal \"<text>\"       let the model plan and act (needs a key)\n");
    printf("  --shot [name.png]      capture the screen now\n");
    printf("  --windows              list interactable windows\n");
    printf("  --pixel x y            colour at a point\n");
    printf("  --find RRGGBB [tol]    locate a colour on screen\n");
    printf("  --move x y             move the cursor\n");
    printf("  --click [b] [x y]      click (left|right|middle|double)\n");
    printf("  --type \"text\"          type into the focused window\n");
    printf("  --key ctrl+s           press a key combination\n");
    printf("  --cursor               print the real cursor position\n");
    printf("  --desktop              print the virtual desktop and monitor geometry\n");
    printf("  --focus <title part>   raise a window\n");
    printf("  --selftest             prove capture + physical input work\n\n");
    printf("Options: --dry-run --delay <ms> --no-restore --key-file <path> --key <key>\n");
    printf("         --auto-run   explicitly allow model-generated scripts to execute\n");
    printf("         --allow-sensitive   bypass per-action prompts (trusted CLI use only)\n");
    printf("         --allow-screen-upload   consent to sending screenshots to the model provider\n");
    printf("         --model <id> --base-url <url> --no-vision --rounds <n> --steps <n>\n");
    printf("         --plan-only   let the model write a script but do not run it\n\n");
    printf("Script commands: shot shotwin region look elements clickel uia move click drag\n");
    printf("                 scroll smove sclick sdrag type key focus waitfocus open close\n");
    printf("                 wait waitclose pixel find findarea verifypixel windows monitors\n");
    printf("                 cursor write mkdir log try done\n");
}

// --------------------------------------------------------------- self-test --
// A real window, a real target, real injected input: if a synthetic click
// lands in this window's message queue, physical interaction genuinely works.
static int g_clicks = 0;
static int g_last_x = -1, g_last_y = -1;
static const UINT_PTR kTargetId = 1;
static HWND g_edit = nullptr;
static wchar_t g_edit_text[256] = {0};

static LRESULT CALLBACK test_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_clicks++;
            g_last_x = LOWORD(lp);
            g_last_y = HIWORD(lp);
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(235, 235, 240));
            SetBkColor(dc, RGB(30, 30, 34));
            static HBRUSH br = CreateSolidBrush(RGB(30, 30, 34));
            return (LRESULT)br;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(55, 55, 60));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            // the target square the injected click must hit
            RECT target{40, 80, 40 + 90, 80 + 90};
            HBRUSH blue = CreateSolidBrush(RGB(30, 120, 255));
            FillRect(dc, &target, blue);
            DeleteObject(blue);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 245));
            RECT text_rc{150, 84, rc.right - 12, 140};
            DrawTextW(dc, L"Hawk OS self-test target\ninjected clicks land here", -1, &text_rc,
                      DT_WORDBREAK);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void pump(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

int run_selftest() {
    enable_dpi_awareness();
    int checks = 0, failed = 0;
    auto check = [&](const char* what, bool ok, const std::string& detail) {
        checks++;
        if (!ok) failed++;
        printf("%-46s %s   %s\n", what, ok ? "PASS" : "FAIL", detail.c_str());
    };

    printf("%s self-test on a %d-bit %dx%d desktop\n\n",
           HAWK_NAME, (int)GetDeviceCaps(GetDC(nullptr), BITSPIXEL),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));

    // 1. capture
    Rect v = virtual_desktop();
    Image img = capture_screen();
    check("capture returns the whole desktop", img.w == v.w && img.h == v.h,
          std::to_string(img.w) + "x" + std::to_string(img.h) + " vs desktop " +
              std::to_string(v.w) + "x" + std::to_string(v.h));
    // count distinct colours to prove it is not a blank buffer
    int distinct = 0;
    uint32_t seen[64];
    for (int y = 0; y < img.h && distinct < 64; y += 7) {
        for (int x = 0; x < img.w && distinct < 64; x += 7) {
            uint32_t c = img.at(x, y);
            bool dup = false;
            for (int i = 0; i < distinct; i++)
                if (seen[i] == c) dup = true;
            if (!dup) seen[distinct++] = c;
        }
    }
    check("capture has real picture content", distinct >= 8,
          std::to_string(distinct) + " distinct colours sampled");

    // 2. png encoder round trip
    std::string shot = take_shot("selftest");
    long fsize = 0;
    int pw = 0, ph = 0;
    {
        FILE* f = fopen(shot.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            fsize = ftell(f);
            fseek(f, 16, SEEK_SET);
            unsigned char hdr[8] = {0};
            if (fread(hdr, 1, 8, f) == 8) {
                pw = (hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
                ph = (hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];
            }
            fclose(f);
        }
    }
    check("screenshot written as valid PNG", fsize > 1000 && pw == img.w && ph == img.h,
          std::to_string(fsize) + " bytes, IHDR " + std::to_string(pw) + "x" + std::to_string(ph));

    // 3. a real window to interact with
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = test_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HawkSelfTest";
    RegisterClassExW(&wc);

    int x = v.x + 120, y = v.y + 120;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HawkSelfTest", L"Hawk OS Self-Test Harness",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, 430, 250,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        check("test window created", false, "CreateWindowEx failed");
        printf("\n%d/%d checks passed\n", checks - failed, checks);
        return failed ? 1 : 0;
    }
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
                             30, 190, 370, 26, hwnd, (HMENU)kTargetId, wc.hInstance, nullptr);
    pump(300);  // let it paint and settle

    // 4. see it
    bool found = false;
    for (const auto& w : list_windows(0))
        if (w.hwnd == hwnd) found = true;
    check("window enumeration sees our window", found, "");

    std::string title = narrow(window_title(hwnd));
    check("window title read back", title.find("Self-Test") != std::string::npos, title);

    HWND by_name = find_window("Self-Test Harness");
    check("find window by title substring", by_name == hwnd, "");

    // 5. window capture
    Image win_img = capture_window(hwnd);
    check("capture a single window", win_img.w > 100 && win_img.h > 100,
          std::to_string(win_img.w) + "x" + std::to_string(win_img.h));

    // 6. focus
    bool focused = focus_window(hwnd);
    pump(120);
    check("bring a window to the front", focused && GetForegroundWindow() == hwnd,
          focused ? "is foreground" : "foreground refused (Windows focus lock)");

    // 7. move the cursor, then click the target: the physical test
    Point target{0, 0};
    {
        POINT client{40 + 45, 80 + 45};
        ClientToScreen(hwnd, &client);
        target = Point{client.x, client.y};
    }
    std::string err;
    Point landed{0, 0};
    bool moved = move_cursor(target.x, target.y, &err, &landed);
    Point obs = cursor_pos();
    pump(150);
    Point later = cursor_pos();
    check("cursor moved to requested pixel",
          moved && abs(landed.x - target.x) <= 2 && abs(landed.y - target.y) <= 2,
          "wanted " + std::to_string(target.x) + "," + std::to_string(target.y) + " OS reported " +
              std::to_string(landed.x) + "," + std::to_string(landed.y) +
              (later.x != obs.x || later.y != obs.y
                   ? ("  (a hand on the mouse has since moved it to " + std::to_string(later.x) +
                      "," + std::to_string(later.y) + ")")
                   : ""));

    int before = g_clicks;
    bool clicked = click_mouse("left", 1, &err);
    pump(250);
    check("injected click reached the window", clicked && g_clicks == before + 1,
          "clicks " + std::to_string(before) + " -> " + std::to_string(g_clicks));
    // the click point must land inside the blue target square (client 40,80 90x90)
    bool inside = g_last_x >= 40 && g_last_x <= 130 && g_last_y >= 80 && g_last_y <= 170;
    check("click landed inside the target square", inside,
          "client " + std::to_string(g_last_x) + "," + std::to_string(g_last_y));

    // 8. colour probe + find on the real screen
    uint32_t blue = 0x00FF781E;  // RGB(30,120,255) in BGR
    uint32_t at_target = pixel_at(target.x, target.y);
    long delta = llabs((long)(at_target & 0xFF) - (long)(blue & 0xFF)) +
                 llabs((long)((at_target >> 8) & 0xFF) - (long)((blue >> 8) & 0xFF)) +
                 llabs((long)((at_target >> 16) & 0xFF) - (long)((blue >> 16) & 0xFF));
    check("pixel probe reports what we drew", delta <= 60,
          "#" + color_hex(at_target) + " vs #" + color_hex(blue));

    Image fresh = capture_screen();
    Point hit{};
    bool found_colour = find_color(fresh, blue, 40, &hit);
    check("find colour on the captured screen", found_colour &&
              abs(hit.x - (target.x - v.x)) < 60 && abs(hit.y - (target.y - v.y)) < 60,
          found_colour ? ("at " + std::to_string(hit.x) + "," + std::to_string(hit.y)) : "not found");

    // 8b. the local vision primitives: whole-region colour search, control
    //     detection and the labelled grid - the parts that let a script see
    //     without a model.
    {
        Rect area;
        bool blob = find_color_area(fresh, blue, 60, &area);
        // `target` is the middle of the blue square, so a correct blob must
        // cover that point and be about the size of the square
        bool covers = blob && area.w >= 60 && area.h >= 60 &&
                      area.contains(target.x - v.x, target.y - v.y);
        check("findarea locates the whole blue square",
              covers,
              blob ? ("region " + std::to_string(area.x) + "," + std::to_string(area.y) + " " +
                      std::to_string(area.w) + "x" + std::to_string(area.h))
                   : "not found");

        std::vector<UiElement> els = detect_elements(fresh, 12);
        bool blue_ranked = false;
        std::string top;
        for (const UiElement& e : els) {
            int d = abs((int)(e.color & 0xFF) - (int)(blue & 0xFF)) +
                    abs((int)((e.color >> 8) & 0xFF) - (int)((blue >> 8) & 0xFF)) +
                    abs((int)((e.color >> 16) & 0xFF) - (int)((blue >> 16) & 0xFF));
            if (d <= 60) blue_ranked = true;
            if (top.empty())
                top = std::string(e.kind) + " at " + std::to_string(e.rect.x) + "," +
                      std::to_string(e.rect.y);
        }
        check("detect_elements ranks the blue control",
              blue_ranked, (std::to_string(els.size()) + " control(s), top: " + top).c_str());

        Image gridded = draw_grid(crop(win_img, 0, 0, win_img.w > 300 ? 300 : win_img.w,
                                       win_img.h > 200 ? 200 : win_img.h), 100);
        bool changed = false;
        if (!gridded.empty()) {
            // a grid line is drawn at x=100 and y=100; those pixels must now
            // differ from the ungridded original
            Image plain = crop(win_img, 0, 0, gridded.w, gridded.h);
            for (int y = 0; y < gridded.h && !changed; y += 5)
                if (gridded.at(100, y) != plain.at(100, y)) changed = true;
            for (int x = 0; x < gridded.w && !changed; x += 5)
                if (gridded.at(x, 100) != plain.at(x, 100)) changed = true;
        }
        check("draw_grid overlays a labelled grid", !gridded.empty() && changed,
              changed ? "grid line drawn" : "no grid line found");
    }

    // 9. typing into a real control
    SetFocus(g_edit);
    focus_window(hwnd);
    SetFocus(g_edit);
    pump(120);
    bool typed = type_text(L"hawk os selftest", &err);
    pump(250);
    GetWindowTextW(g_edit, g_edit_text, 255);
    // The physical keyboard shares this window while the test runs, so require
    // every typed character to arrive in order rather than an exact match.
    std::wstring got(g_edit_text), want = L"hawk os selftest";
    size_t wi = 0;
    for (wchar_t c : got)
        if (wi < want.size() && c == want[wi]) wi++;
    check("typed text arrived in the edit control", typed && wi == want.size(),
          std::to_string(wi) + "/" + std::to_string(want.size()) + " characters in order: '" +
              narrow(got) + "'");

    bool combo = press_combo("ctrl+a", &err);
    pump(150);
    check("key combination accepted", combo, "");

    // 10. scrolling must not throw
    bool scrolled = scroll_wheel(-2, &err);
    pump(100);
    check("wheel event accepted", scrolled, "");

    // 11. script engine: dry-run must not touch the machine
    {
        std::string probe = app_dir() + "\\workspace\\selftest.txt";
        DeleteFileA(probe.c_str());
        Config cfg;
        cfg.dry_run = true;
        cfg.restore_cursor = false;
        cfg.step_delay_ms = 0;
        Agent agent(cfg);
        bool ok = agent.run_script("move 5 5\nclick left\nshot\nwrite selftest.txt hello");
        pump(150);
        // The physical mouse may drift on its own, so assert the meaningful
        // thing: the script's target was not visited and nothing was written.
        Point after = cursor_pos();
        bool not_moved = !(abs(after.x - 5) <= 3 && abs(after.y - 5) <= 3);
        bool no_file = GetFileAttributesA(probe.c_str()) == INVALID_FILE_ATTRIBUTES;
        check("dry-run script performs nothing", ok && not_moved && no_file,
              ok ? (std::string("cursor at ") + std::to_string(after.x) + "," +
                    std::to_string(after.y) + " (dry-run target 5,5 untouched), no file written")
                 : "script reported a failure");
        DeleteFileA(probe.c_str());
    }

    // 12. the script language really acts when it is not a dry run. The
    //     position the step itself read back must be exactly the target.
    {
        Config cfg;
        cfg.restore_cursor = false;
        cfg.step_delay_ms = 0;
        Agent agent(cfg);
        Point want{200, 180};
        bool ok = agent.run_script("move " + std::to_string(want.x) + " " + std::to_string(want.y));
        std::string reported;
        for (const auto& l : agent.history())
            if (l.text.rfind("cursor -> ", 0) == 0) reported = l.text.substr(strlen("cursor -> "));
        check("script moves the real cursor",
              ok && reported == std::to_string(want.x) + "," + std::to_string(want.y),
              "target " + std::to_string(want.x) + "," + std::to_string(want.y) +
                  ", script read back '" + reported + "'");
    }

    DestroyWindow(hwnd);
    pump(100);
    printf("\n%d/%d checks passed\n", checks - failed, checks);
    if (!failed) printf("all good: Hawk OS can see this screen and physically drive it.\n");
    return failed ? 1 : 0;
}

// -------------------------------------------------------------------- CLI ---
static bool run_file_script(Agent& agent, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        printf("cannot open script %s\n", path.c_str());
        return false;
    }
    std::string body;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);
    return agent.run_script(body);
}

int run_cli(int argc, char** argv) {
    Config cfg;
    load_settings(&cfg);
    std::string command, arg1, arg2;
    int positional = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                printf("%s needs a value\n", what);
                return "";
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h" || a == "/?") {
            usage();
            return 0;
        } else if (a == "--dry-run") cfg.dry_run = true;
        else if (a == "--auto-run") cfg.auto_run = true;
        else if (a == "--allow-sensitive") cfg.allow_sensitive_actions = true;
        else if (a == "--allow-screen-upload") cfg.allow_screen_upload = true;
        else if (a == "--no-restore") cfg.restore_cursor = false;
        else if (a == "--no-vision") cfg.vision = false;
        else if (a == "--plan-only") cfg.auto_run = false;
        else if (a == "--vision") cfg.vision = true;
        else if (a == "--delay") { std::string v = next("--delay"); if (!v.empty()) cfg.step_delay_ms = atoi(v.c_str()); }
        else if (a == "--rounds") { std::string v = next("--rounds"); if (!v.empty()) cfg.max_rounds = atoi(v.c_str()); }
        else if (a == "--steps") { std::string v = next("--steps"); if (!v.empty()) cfg.max_steps = atoi(v.c_str()); }
        else if (a == "--model") cfg.model = next("--model");
        else if (a == "--base-url") cfg.base_url = next("--base-url");
        else if (a == "--key") cfg.api_key = next("--key");
        else if (a == "--key-file") {
            std::string p = next("--key-file");
            FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "rb");
            if (!f) {
                printf("cannot read key file %s\n", p.c_str());
                return 2;
            }
            char line[512] = {0};
            if (!fgets(line, sizeof(line), f)) line[0] = '\0';
            fclose(f);
            cfg.api_key = trim(line);
        } else if (!command.empty()) {
            if (positional == 0) arg1 = a;
            else if (positional == 1) arg2 = a;
            positional++;
        } else {
            command = a;
        }
    }

    if (command.empty()) {
        usage();
        return 0;
    }

    enable_dpi_awareness();

    if (command == "--selftest" || command == "selftest") return run_selftest();

    Agent agent(cfg);
    agent.on_log = [](const LogLine& l) { printf("[%-6s] %s\n", l.level.c_str(), l.text.c_str()); };

    if (command == "--shot") {
        std::string path = arg1.empty() ? take_shot("cli") : arg1;
        if (arg1.empty()) {
            if (path.empty()) {
                printf("capture failed\n");
                return 1;
            }
        } else {
            Image img = capture_screen();
            if (!save_png(img, path)) {
                printf("could not write %s\n", path.c_str());
                return 1;
            }
        }
        printf("%s\n", path.c_str());
        return 0;
    }
    if (command == "--windows") { printf("%s", describe_windows(arg1.empty() ? 40 : atoi(arg1.c_str())).c_str()); return 0; }
    if (command == "--cursor") {
        Point p = cursor_pos();
        printf("cursor %d %d\n", p.x, p.y);
        return 0;
    }
    if (command == "--desktop") {
        Rect v = virtual_desktop();
        printf("virtual %d %d %d %d\n", v.x, v.y, v.w, v.h);
        for (const Rect& m : monitor_rects())
            printf("monitor %d %d %d %d\n", m.x, m.y, m.w, m.h);
        HDC dc = GetDC(nullptr);
        printf("bits %d\n", GetDeviceCaps(dc, BITSPIXEL));
        ReleaseDC(nullptr, dc);
        return 0;
    }
    if (command == "--pixel") { agent.run_script("pixel " + arg1 + " " + arg2); return 0; }
    if (command == "--find") { agent.run_script("find " + arg1 + (arg2.empty() ? "" : " " + arg2)); return 0; }
    if (command == "--move") { agent.run_script("move " + arg1 + " " + arg2); return 0; }
    if (command == "--click") {
        std::string btn = arg1.empty() ? "left" : arg1;
        agent.run_script("click " + btn);
        return 0;
    }
    if (command == "--type") { agent.run_script("type \"" + arg1 + "\""); return 0; }
    if (command == "--key") { agent.run_script("key " + arg1); return 0; }
    if (command == "--focus") { agent.run_script("focus \"" + arg1 + "\""); return 0; }
    if (command == "--run") return agent.run_script(arg1) ? 0 : 1;
    if (command == "--file") return run_file_script(agent, arg1) ? 0 : 1;
    if (command == "--goal") return agent.run_goal(arg1) ? 0 : 1;

    printf("unknown command '%s'\n\n", command.c_str());
    usage();
    return 2;
}

}  // namespace hawk

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(hawk::narrow(wargv[i]));
    if (wargv) LocalFree(wargv);

    if (argc > 1) {
        hawk::attach_console();
        std::vector<char*> raw;
        for (auto& s : args) raw.push_back(const_cast<char*>(s.c_str()));
        return hawk::run_cli((int)raw.size(), raw.data());
    }
    hawk::Config cfg;
    hawk::load_settings(&cfg);
    return hawk::run_gui(inst, cfg);
}

int main() {  // for tooling that links a console entry point
    return WinMain(GetModuleHandleW(nullptr), nullptr, nullptr, SW_SHOWNORMAL);
}
