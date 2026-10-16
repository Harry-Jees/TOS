// Hawk OS - the Agent. It executes a small command language against the real
// machine (move/click/type/focus/capture/find/write), and it can ask a vision
// model what to do next by handing it a fresh screenshot.
#include "hawk.h"

#include <shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace hawk {

// ------------------------------------------------------------------ logging --
static std::string log_path() { return app_dir() + "\\hawk.log"; }

void Agent::log(const std::string& level, const std::string& text) {
    LogLine line{level, text};
    history_.push_back(line);
    if (history_.size() > 4000) history_.erase(history_.begin(), history_.begin() + 1000);

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILE* f = fopen(log_path().c_str(), "a");
    if (f) {
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d [%-6s] %s\n", st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, level.c_str(), text.c_str());
        fclose(f);
    }
    if (on_log) on_log(line);
}

Agent::Agent(Config cfg) : config(cfg) {
    enable_dpi_awareness();
    log("info", std::string(HAWK_NAME) + " " + HAWK_VERSION + " agent ready (" +
                    std::to_string(monitor_rects().size()) + " monitor(s), virtual desktop " +
                    std::to_string(virtual_desktop().w) + "x" + std::to_string(virtual_desktop().h) +
                    ")");
    if (config.dry_run) log("warn", "dry-run mode: actions are logged, not performed");
}

Agent::~Agent() = default;

static void sleep_ms(int ms) {
    // Awake often enough that ESC / the STOP button is noticed quickly.
    while (ms > 0) {
        int slice = ms > 50 ? 50 : ms;
        Sleep(slice);
        ms -= slice;
        if (abort_requested()) return;
    }
}

// ------------------------------------------------------------------ helpers --
static std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            if (c == 'n') out.push_back('\n');
            else if (c == 't') out.push_back('\t');
            else if (c == 'r') out.push_back('\r');
            else if (c == '\\') out.push_back('\\');
            else if (c == '"') out.push_back('"');
            else { out.push_back('\\'); out.push_back(c); }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static bool is_absolute(const std::string& p) {
    return p.size() >= 2 && p[1] == ':' && isalpha((unsigned char)p[0]);
}

// Everything after the n-th token of the original line, quotes stripped. This
// keeps `type hello world` and `write notes.txt two words` intact instead of
// handing the command only its first word.
static std::string remainder_after(const std::string& line, int after_token) {
    size_t i = 0, n = line.size();
    for (int skipped = 0; skipped < after_token; skipped++) {
        while (i < n && isspace((unsigned char)line[i])) i++;
        if (i >= n) return "";
        if (line[i] == '"') {
            i++;
            while (i < n && line[i] != '"') i++;
            if (i < n) i++;
        } else {
            while (i < n && !isspace((unsigned char)line[i])) i++;
        }
    }
    std::string rest = trim(line.substr(i));
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
    return rest;
}

static std::string expand_path(const std::string& raw) {
    if (is_absolute(raw)) return raw;
    std::string dir = app_dir() + "\\workspace";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\" + raw;
}

static void substitute(std::string& line, const Point& found, bool has_found,
                       const std::string& shot) {
    auto repl = [&line](const std::string& what, const std::string& with) {
        size_t pos;
        while ((pos = line.find(what)) != std::string::npos) line.replace(pos, what.size(), with);
    };
    if (has_found) {
        repl("$fx", std::to_string(found.x));
        repl("$fy", std::to_string(found.y));
    }
    if (!shot.empty()) repl("$shot", shot);
}

// Model- and planner-generated scripts are shown in the app and kept on disk,
// so "the agent wrote a script and ran it" leaves something you can re-run.
void Agent::publish_script(const std::string& script) {
    if (script.empty()) return;
    if (on_script) on_script(script);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[160];
    snprintf(name, sizeof(name), "agent-script-%04d%02d%02d-%02d%02d%02d.hawk", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string dir = app_dir() + "\\workspace";
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = dir + "\\" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(script.data(), 1, script.size(), f);
        fclose(f);
        log("agent", "script saved for reuse: " + path);
    }
}

// ------------------------------------------------------------------- script --
bool Agent::run_script(const std::string& script) {
    Point cursor_before = cursor_pos();
    if (config.restore_cursor) save_cursor_origin();
    request_abort(false);
    stop_ = false;

    log("info", "run start: " + std::to_string(split_words(script).size()) + " token(s) of input");
    int executed = 0, failed = 0;
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t nl = script.find('\n', pos);
        std::string line = script.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (nl == std::string::npos) pos = script.size() + 1;
        else pos = nl + 1;

        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t == "done") {
            log("ok", "script signalled done");
            break;
        }
        if (abort_requested()) {
            log("warn", "aborted by user (ESC or STOP)");
            failed++;
            break;
        }
        if (config.step_delay_ms > 0) sleep_ms(config.step_delay_ms);
        executed++;
        if (!step(t, 0)) {
            failed++;
            log("fail", "step failed: " + t);
            if (config.dry_run) continue;
            log("warn", "stopping: a failed step means the state is no longer what the script expects");
            break;
        }
    }
    if (config.restore_cursor) restore_cursor_origin();
    char buf[160];
    snprintf(buf, sizeof(buf), "run end: %d step(s), %d failure(s)", executed, failed);
    log(failed ? "warn" : "ok", buf);
    Point after = cursor_pos();
    if (after.x != cursor_before.x || after.y != cursor_before.y)
        log("info", "cursor moved " + std::to_string(cursor_before.x) + "," +
                        std::to_string(cursor_before.y) + " -> " + std::to_string(after.x) + "," +
                        std::to_string(after.y));
    return failed == 0;
}

// One command. The language is deliberately tiny and explicit.
bool Agent::step(const std::string& raw_line, int depth) {
    std::string line = raw_line;
    Point found{};
    bool has_found = false;
    substitute(line, found, has_found, last_shot_);

    std::vector<std::string> a = split_words(line);
    if (a.empty()) return true;
    std::string cmd = to_lower(a[0]);
    std::string err;

    auto need = [&](size_t n) -> bool {
        if (a.size() < n + 1) {
            log("fail", cmd + ": needs at least " + std::to_string(n) + " argument(s)");
            return false;
        }
        return true;
    };
    auto number = [&](size_t i, int dflt) -> int {
        if (a.size() <= i) return dflt;
        try {
            return std::stoi(a[i]);
        } catch (...) {
            return dflt;
        }
    };
    auto rest = [&](size_t i) -> std::string {
        // the whole tail of the line after i tokens, quotes stripped
        return remainder_after(line, (int)i);
    };

    // ---- seeing ----
    if (cmd == "shot") {
        if (config.dry_run) {
            log("action", "shot (dry-run)");
            return true;
        }
        Image img = capture_screen();
        if (img.empty()) {
            log("fail", "capture returned no pixels");
            return false;
        }
        std::string path = a.size() > 1 && !a[1].empty()
                               ? (is_absolute(a[1]) ? a[1] : shot_dir() + "\\" + a[1])
                               : take_shot("screen");
        if (a.size() > 1 && !a[1].empty()) {
            if (!save_png(img, path)) {
                log("fail", "could not write " + path);
                return false;
            }
        }
        last_shot_ = path;
        log("ok", "screenshot " + std::to_string(img.w) + "x" + std::to_string(img.h) + " -> " + path);
        return true;
    }
    if (cmd == "shotwin") {
        if (!need(1)) return false;
        HWND hwnd = find_window(a[1]);
        if (!hwnd) {
            log("fail", "shotwin: no window matching '" + a[1] + "'");
            return false;
        }
        if (config.dry_run) {
            log("action", "shotwin " + a[1] + " (dry-run)");
            return true;
        }
        Image img = capture_window(hwnd);
        if (img.empty()) {
            log("fail", "shotwin: capture failed");
            return false;
        }
        std::string path = shot_dir() + "\\win-" + a[1].substr(0, 24) + ".png";
        if (a.size() > 2 && !a[2].empty()) path = is_absolute(a[2]) ? a[2] : shot_dir() + "\\" + a[2];
        for (char& c : path)
            if (!isalnum((unsigned char)c) && strchr("\\/._:-", c) == nullptr) c = '-';
        if (!save_png(img, path)) {
            log("fail", "could not write " + path);
            return false;
        }
        last_shot_ = path;
        log("ok", "window shot " + std::to_string(img.w) + "x" + std::to_string(img.h) + " -> " + path);
        return true;
    }
    if (cmd == "region") {
        if (!need(4)) return false;
        Rect r{number(1, 0), number(2, 0), number(3, 100), number(4, 100)};
        if (config.dry_run) {
            log("action", "region (dry-run)");
            return true;
        }
        Image img = capture_region(r);
        if (img.empty()) {
            log("fail", "region: nothing to capture");
            return false;
        }
        std::string path = take_shot("region");
        if (!save_png(img, path)) {
            log("fail", "could not write " + path);
            return false;
        }
        last_shot_ = path;
        log("ok", "region " + std::to_string(img.w) + "x" + std::to_string(img.h) + " -> " + path);
        return true;
    }

    // ---- acting ----
    if (cmd == "move") {
        int x, y;
        if (a.size() > 1 && to_lower(a[1]) == "rel") {
            if (!need(3)) return false;
            x = number(2, 0);
            y = number(3, 0);
            if (config.dry_run) {
                log("action", "move rel " + std::to_string(x) + "," + std::to_string(y) + " (dry-run)");
                return true;
            }
            if (!move_cursor_relative(x, y, &err)) {
                log("fail", "move rel: " + err);
                return false;
            }
            Point p = cursor_pos();
            log("ok", "cursor -> " + std::to_string(p.x) + "," + std::to_string(p.y));
            return true;
        }
        if (!need(2)) return false;
        x = number(1, 0);
        y = number(2, 0);
        if (config.dry_run) {
            log("action", "move " + std::to_string(x) + "," + std::to_string(y) + " (dry-run)");
            return true;
        }
        if (!move_cursor(x, y, &err)) {
            log("fail", "move: " + err);
            return false;
        }
        log("ok", "cursor -> " + std::to_string(x) + "," + std::to_string(y));
        return true;
    }
    if (cmd == "click") {
        std::string button = "left";
        int times = 1;
        int x = INT32_MIN, y = INT32_MIN;
        size_t i = 1;
        if (i < a.size()) {
            std::string t = to_lower(a[i]);
            if (t == "double") {
                times = 2;
                i++;
            } else if (t == "left" || t == "right" || t == "middle") {
                button = t;
                i++;
            } else if (t == "triple") {
                times = 3;
                i++;
            }
        }
        if (i + 1 < a.size()) {
            x = number(i, 0);
            y = number(i + 1, 0);
        }
        std::string where = (x == INT32_MIN) ? "at cursor"
                                             : ("at " + std::to_string(x) + "," + std::to_string(y));
        if (config.dry_run) {
            log("action", "click " + button + (times > 1 ? " x" + std::to_string(times) : "") + " " +
                              where + " (dry-run)");
            return true;
        }
        if (x != INT32_MIN && !move_cursor(x, y, &err)) {
            log("fail", "click: " + err);
            return false;
        }
        if (!click_mouse(button, times, &err)) {
            log("fail", "click: " + err);
            return false;
        }
        Point p = cursor_pos();
        log("ok", (times > 1 ? std::to_string(times) + "x " : "") + button + " click at " +
                      std::to_string(p.x) + "," + std::to_string(p.y));
        return true;
    }
    if (cmd == "drag") {
        if (!need(4)) return false;
        int x1 = number(1, 0), y1 = number(2, 0), x2 = number(3, 0), y2 = number(4, 0);
        if (config.dry_run) {
            log("action", "drag (dry-run)");
            return true;
        }
        if (!drag_mouse(x1, y1, x2, y2, "left", &err)) {
            log("fail", "drag: " + err);
            return false;
        }
        log("ok", "dragged " + std::to_string(x1) + "," + std::to_string(y1) + " -> " +
                      std::to_string(x2) + "," + std::to_string(y2));
        return true;
    }
    if (cmd == "scroll") {
        int n = need(1) ? number(1, -3) : -3;
        if (config.dry_run) {
            log("action", "scroll (dry-run)");
            return true;
        }
        if (!scroll_wheel(n, &err)) {
            log("fail", "scroll: " + err);
            return false;
        }
        log("ok", "scrolled " + std::to_string(n) + " notch(es)");
        return true;
    }
    if (cmd == "type") {
        if (!need(1)) return false;
        std::string text = unescape(rest(1));
        if (config.dry_run) {
            log("action", "type \"" + text + "\" (dry-run)");
            return true;
        }
        if (!type_text(wide(text), &err)) {
            log("fail", "type: " + err);
            return false;
        }
        log("ok", "typed " + std::to_string(text.size()) + " char(s): " +
                      text.substr(0, 60).append(text.size() > 60 ? "..." : ""));
        return true;
    }
    if (cmd == "key") {
        if (!need(1)) return false;
        if (config.dry_run) {
            log("action", "key " + a[1] + " (dry-run)");
            return true;
        }
        if (!press_combo(a[1], &err)) {
            log("fail", "key: " + err);
            return false;
        }
        log("ok", "pressed " + a[1]);
        return true;
    }
    if (cmd == "focus" || cmd == "open" || cmd == "close") {
        if (!need(1)) return false;
        if (cmd == "open") {
            std::string target = rest(1);
            if (config.dry_run) {
                log("action", "open " + target + " (dry-run)");
                return true;
            }
            HINSTANCE r = ShellExecuteW(nullptr, L"open", wide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) {
                log("fail", "open: shell could not launch '" + target + "' (code " +
                                std::to_string((INT_PTR)r) + ")");
                return false;
            }
            log("ok", "launched " + target);
            return true;
        }
        HWND hwnd = find_window(a[1]);
        if (!hwnd) {
            log("fail", cmd + ": no window matching '" + a[1] + "'");
            return false;
        }
        if (config.dry_run) {
            log("action", cmd + " " + a[1] + " (dry-run)");
            return true;
        }
        if (cmd == "close") {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            log("ok", "asked '" + a[1] + "' to close");
            return true;
        }
        if (!focus_window(hwnd)) {
            log("fail", "focus: Windows refused to raise '" + a[1] + "'");
            return false;
        }
        log("ok", "focused \"" + narrow(window_title(hwnd)) + "\"");
        return true;
    }
    if (cmd == "wait" || cmd == "sleep") {
        int ms = need(1) ? number(1, 500) : 500;
        log("info", (config.dry_run ? "(dry-run) " : "") + std::string("waiting ") +
                        std::to_string(ms) + " ms");
        if (!config.dry_run) sleep_ms(ms);
        return true;
    }

    // ---- inspecting ----
    if (cmd == "pixel") {
        if (!need(2)) return false;
        int x = number(1, 0), y = number(2, 0);
        uint32_t c = pixel_at(x, y);
        if (c == 0xFFFFFFFFu) {
            log("fail", "pixel: (" + std::to_string(x) + "," + std::to_string(y) +
                            ") is outside the screen");
            return false;
        }
        log("ok", "pixel " + std::to_string(x) + "," + std::to_string(y) + " = #" + color_hex(c));
        return true;
    }
    if (cmd == "find") {
        if (!need(1)) return false;
        uint32_t want = parse_color(a[1]);
        if (want == 0xFFFFFFFFu) {
            log("fail", "find: '" + a[1] + "' is not an RRGGBB colour");
            return false;
        }
        int tol = a.size() > 2 ? number(2, 0) : 0;
        std::string action = a.size() > 3 ? to_lower(a[3]) : "";
        Image img = capture_screen();
        Point pt{};
        if (!find_color(img, want, tol, &pt)) {
            log("fail", "find: #" + to_lower(a[1]) + " not on screen (tolerance " +
                            std::to_string(tol) + ")");
            return false;
        }
        // capture is virtual-desktop relative; input needs absolute screen pixels
        Rect v = virtual_desktop();
        Point abs_pt{pt.x + v.x, pt.y + v.y};
        log("ok", "find: #" + to_lower(a[1]) + " at screen " + std::to_string(abs_pt.x) + "," +
                      std::to_string(abs_pt.y));
        if (action == "move" || action == "click" || action == "double") {
            if (config.dry_run) {
                log("action", action + " found point (dry-run)");
                return true;
            }
            if (!move_cursor(abs_pt.x, abs_pt.y, &err)) {
                log("fail", "find: " + err);
                return false;
            }
            if (action != "move") {
                int times = action == "double" ? 2 : 1;
                if (!click_mouse("left", times, &err)) {
                    log("fail", "find: " + err);
                    return false;
                }
                log("ok", action + " at the found colour");
            }
        }
        return true;
    }
    if (cmd == "verifypixel") {
        if (!need(3)) return false;
        int x = number(1, 0), y = number(2, 0);
        uint32_t want = parse_color(a[3]);
        int tol = a.size() > 4 ? number(4, 10) : 10;
        uint32_t got = pixel_at(x, y);
        if (got == 0xFFFFFFFFu) {
            log("fail", "verifypixel: off screen");
            return false;
        }
        long d = llabs((long)(got & 0xFF) - (long)(want & 0xFF)) +
                 llabs((long)((got >> 8) & 0xFF) - (long)((want >> 8) & 0xFF)) +
                 llabs((long)((got >> 16) & 0xFF) - (long)((want >> 16) & 0xFF));
        if (d > tol) {
            log("fail", "verifypixel: wanted #" + to_lower(a[3]) + ", screen shows #" +
                            color_hex(got) + " (delta " + std::to_string(d) + ")");
            return false;
        }
        log("ok", "verifypixel: #" + color_hex(got) + " matches expectation");
        return true;
    }
    if (cmd == "windows") {
        int limit = a.size() > 1 ? number(1, 40) : 40;
        log("info", "windows:\n" + describe_windows(limit));
        return true;
    }
    if (cmd == "cursor") {
        Point p = cursor_pos();
        log("ok", "cursor at " + std::to_string(p.x) + "," + std::to_string(p.y));
        return true;
    }
    if (cmd == "monitors") {
        auto mons = monitor_rects();
        Rect v = virtual_desktop();
        std::string s = "virtual desktop " + std::to_string(v.w) + "x" + std::to_string(v.h) +
                        " at " + std::to_string(v.x) + "," + std::to_string(v.y) + "\n";
        for (size_t i = 0; i < mons.size(); i++)
            s += "  monitor " + std::to_string(i) + ": " + std::to_string(mons[i].w) + "x" +
                 std::to_string(mons[i].h) + " at " + std::to_string(mons[i].x) + "," +
                 std::to_string(mons[i].y) + "\n";
        log("info", s);
        return true;
    }

    // ---- files ----
    if (cmd == "write") {
        if (!need(1)) return false;
        std::string path = expand_path(a[1]);
        std::string body = a.size() > 2 ? unescape(rest(2)) : "";
        if (config.dry_run) {
            log("action", "write " + path + " (" + std::to_string(body.size()) + " bytes, dry-run)");
            return true;
        }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            log("fail", "write: cannot open " + path);
            return false;
        }
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
        log("ok", "wrote " + std::to_string(body.size()) + " bytes to " + path);
        return true;
    }
    if (cmd == "read") {
        if (!need(1)) return false;
        std::string path = expand_path(a[1]);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            log("fail", "read: cannot open " + path);
            return false;
        }
        std::string body;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
        fclose(f);
        log("ok", "read " + path + " (" + std::to_string(body.size()) + " bytes):\n" +
                      body.substr(0, 2000));
        return true;
    }
    if (cmd == "mkdir") {
        if (!need(1)) return false;
        std::string path = expand_path(a[1]);
        if (config.dry_run) {
            log("action", "mkdir " + path + " (dry-run)");
            return true;
        }
        if (!CreateDirectoryA(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            log("fail", "mkdir: " + path + " failed (error " + std::to_string(GetLastError()) + ")");
            return false;
        }
        log("ok", "directory ready: " + path);
        return true;
    }

    // ---- misc ----
    if (cmd == "try") {
        std::string inner = trim(line.substr(line.find_first_of(" \t") + 1));
        if (inner.empty()) return true;
        bool ok = step(inner, depth + 1);
        if (!ok) log("warn", "try: ignoring failure of: " + inner);
        return true;  // never propagates a failure
    }
    if (cmd == "log" || cmd == "note" || cmd == "echo") {
        log("agent", unescape(a.size() > 1 ? rest(1) : ""));
        return true;
    }
    if (cmd == "done") return true;

    log("fail", "unknown command '" + cmd + "'");
    if (depth == 0 && to_lower(line) == to_lower(trim(line)) && a.size() == 1 &&
        cmd.find_first_of(" \t") == std::string::npos)
        log("info", "that looks like plain words, not a command - press Run goal to hand a "
                    "sentence to the agent, or Run script for commands");
    log("info", "commands: shot shotwin region move click drag scroll type key focus open close "
                "wait pixel find verifypixel windows cursor monitors write read mkdir log try done");
    return false;
}

// --------------------------------------------------------------- llm brain --
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

// Pull the "content" string out of an OpenAI-compatible chat completion.
static std::string extract_content(const std::string& body) {
    size_t key = body.find("\"content\"");
    while (key != std::string::npos) {
        size_t colon = body.find(':', key);
        if (colon == std::string::npos) break;
        size_t i = colon + 1;
        while (i < body.size() && isspace((unsigned char)body[i])) i++;
        if (i < body.size() && body[i] == '"') {
            std::string out;
            for (i++; i < body.size(); i++) {
                char c = body[i];
                if (c == '\\' && i + 1 < body.size()) {
                    char n = body[++i];
                    if (n == 'n') out.push_back('\n');
                    else if (n == 't') out.push_back('\t');
                    else if (n == 'r') out.push_back('\r');
                    else if (n == 'u') {
                        if (i + 4 < body.size()) {
                            int code = (int)strtol(body.substr(i + 1, 4).c_str(), nullptr, 16);
                            i += 4;
                            if (code < 128) out.push_back((char)code);
                            else out += "?";  // non-latin escapes are rare in command output
                        }
                    } else {
                        out.push_back(n);
                    }
                } else if (c == '"') {
                    return out;
                } else {
                    out.push_back(c);
                }
            }
            return out;
        }
        key = body.find("\"content\"", key + 1);
    }
    return "";
}

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = 443;
    std::wstring path;
    bool secure = true;
    bool ok = false;
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl out;
    std::string s = url;
    if (s.rfind("https://", 0) == 0) {
        out.secure = true;
        s = s.substr(8);
    } else if (s.rfind("http://", 0) == 0) {
        out.secure = false;
        s = s.substr(7);
        out.port = 80;
    } else {
        s = s.substr(s.find("//") == 0 ? 2 : 0);
    }
    size_t slash = s.find('/');
    std::string hostpart = slash == std::string::npos ? s : s.substr(0, slash);
    out.path = wide(slash == std::string::npos ? "/" : s.substr(slash));
    size_t colon = hostpart.find(':');
    if (colon != std::string::npos) {
        out.port = (INTERNET_PORT)atoi(hostpart.substr(colon + 1).c_str());
        hostpart = hostpart.substr(0, colon);
    }
    out.host = wide(hostpart);
    out.ok = !out.host.empty();
    return out;
}

bool Agent::llm_chat(const std::string& prompt, const std::string& image_b64,
                     std::string* reply, std::string* err) {
    ParsedUrl url = parse_url(config.base_url);
    if (!url.ok) {
        *err = "base URL looks wrong: " + config.base_url;
        return false;
    }
    if (config.api_key.empty()) {
        *err = "no API key set";
        return false;
    }

    std::string content;
    if (!image_b64.empty()) {
        content = "[{\"type\":\"text\",\"text\":\"" + json_escape(prompt) +
                  "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," +
                  image_b64 + "\"}}]";
    } else {
        content = "\"" + json_escape(prompt) + "\"";
    }
    std::string body = "{\"model\":\"" + json_escape(config.model) + "\",\"messages\":[{\"role\":\"" +
                       std::string("user") + "\",\"content\":" + content +
                       "}],\"temperature\":0.2,\"max_tokens\":700,\"stream\":false}";

    HINTERNET session = WinHttpOpen(L"HawkOS/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *err = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 30000, 120000, 120000);

    HINTERNET conn = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (!conn) {
        *err = "cannot reach " + narrow(url.host) + ":" + std::to_string(url.port) + " (" +
               std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(session);
        return false;
    }
    // The configured base URL is an API root (".../v1"); the endpoint is
    // always chat/completions unless the full path was given.
    std::wstring req_path = url.path;
    const std::wstring suffix = L"/chat/completions";
    if (req_path.size() < suffix.size() ||
        req_path.compare(req_path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        if (!req_path.empty() && req_path.back() == L'/') req_path.pop_back();
        req_path += suffix;
    }

    DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", req_path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        *err = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " +
                           wide(config.api_key) + L"\r\n";
    BOOL sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L,
                                   (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) {
        *err = "request failed (" + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    BOOL received = WinHttpReceiveResponse(req, nullptr);
    if (!received) {
        *err = "no response (" + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    std::string data;
    DWORD avail = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
        chunk.resize(read);
        data += chunk;
        if (data.size() > 8u * 1024 * 1024) break;
    } while (avail > 0);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    if (status != 200) {
        std::string snippet = data.substr(0, 300);
        *err = "HTTP " + std::to_string(status) + ": " + snippet;
        return false;
    }
    std::string text = extract_content(data);
    if (text.empty()) {
        *err = "model reply had no message content: " + data.substr(0, 200);
        return false;
    }
    *reply = text;
    return true;
}

std::vector<std::string> Agent::parse_commands(const std::string& reply) {
    // Accept a fenced block or bare lines; tolerate bullets and numbering.
    std::vector<std::string> out;
    std::string text = reply;
    size_t fence = text.find("```");
    if (fence != std::string::npos) {
        size_t start = text.find('\n', fence);
        size_t end = text.find("```", start == std::string::npos ? fence + 3 : start);
        if (start != std::string::npos && end != std::string::npos && end > start)
            text = text.substr(start + 1, end - start - 1);
    }
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        std::string t = trim(line);
        while (!t.empty() && (t[0] == '-' || t[0] == '*' || t[0] == '`' || t[0] == '>')) {
            t = trim(t.substr(1));
        }
        if (t.size() > 2 && isdigit((unsigned char)t[0]) && (t[1] == '.' || t[1] == ')'))
            t = trim(t.substr(2));
        if (t.empty() || t[0] == '#') continue;
        std::vector<std::string> w = split_words(t);
        if (w.empty()) continue;
        std::string c = to_lower(w[0]);
        if (c.back() == ':') c.pop_back();
        static const char* verbs[] = {"shot", "shotwin", "region", "move", "click", "drag",
                                     "scroll", "type", "key", "focus", "open", "close", "wait",
                                     "sleep", "pixel", "find", "verifypixel", "windows", "cursor",
                                     "monitors", "write", "read", "mkdir", "log", "note", "echo",
                                     "try", "done"};
        bool known = false;
        for (const char* v : verbs)
            if (c == v) known = true;
        if (!known) continue;
        // rebuild the line with the cleaned verb
        out.push_back(c + t.substr(w[0].size()));
    }
    return out;
}

std::string Agent::screen_summary() {
    Rect v = virtual_desktop();
    Point p = cursor_pos();
    std::string s;
    s += "Screen: virtual desktop " + std::to_string(v.w) + "x" + std::to_string(v.h) + " with the "
         "top-left corner at (" + std::to_string(v.x) + "," + std::to_string(v.y) +
         "). All command coordinates are absolute pixels in this space.\n";
    auto mons = monitor_rects();
    for (size_t i = 0; i < mons.size(); i++)
        s += "Monitor " + std::to_string(i) + ": " + std::to_string(mons[i].w) + "x" +
             std::to_string(mons[i].h) + " at " + std::to_string(mons[i].x) + "," +
             std::to_string(mons[i].y) + "\n";
    s += "Cursor: " + std::to_string(p.x) + "," + std::to_string(p.y) + "\n";
    s += "Windows:\n" + describe_windows(28);
    return s;
}

static std::string command_reference() {
    return
        "shot [name.png]                  capture the whole screen\n"
        "shotwin <title-part> [name.png]  capture one window\n"
        "region x y w h                   capture part of the screen\n"
        "move x y | move rel dx dy        move the real cursor\n"
        "click [left|right|double|triple] [x y]   real click (optionally after moving)\n"
        "drag x1 y1 x2 y2                 press, move, release\n"
        "scroll <notches>                 wheel (negative = down)\n"
        "type <text>                      type text (\\n = Enter), real keystrokes\n"
        "key <combo>                      e.g. key ctrl+s, key alt+f4, key enter\n"
        "focus <title-part>               bring a window to the front\n"
        "open <path|url|app>              launch it via the shell\n"
        "close <title-part>               ask a window to close\n"
        "wait <ms>                        pause\n"
        "pixel x y                        report the colour at a point\n"
        "find RRGGBB [tol] [move|click|double]   locate a colour, optionally act on it\n"
        "verifypixel x y RRGGBB [tol]     assert what is on screen\n"
        "windows | monitors | cursor      describe the machine\n"
        "write <path> <text> | mkdir <path>   create files/directories\n"
        "log <text>                       write a note into the run log\n"
        "done                             finish (say this when the goal is met)\n";
}

static bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back((char)c);
        else if (c == ' ') out.push_back('+');
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

// The rule-based planner. It is deliberately small and predictable: it is the
// safety net for machines with no model configured, not a replacement for one.
std::vector<std::string> Agent::plan_locally(const std::string& goal) {
    struct AppAlias {
        const char* alias;
        const char* command;
        const char* focus_hint;
    };
    static const AppAlias apps[] = {
        {"notepad", "notepad", "Notepad"},        {"text editor", "notepad", "Notepad"},
        {"calculator", "calc", "Calculator"},      {"calc", "calc", "Calculator"},
        {"paint", "mspaint", "Paint"},            {"file explorer", "explorer", "File Explorer"},
        {"explorer", "explorer", "File Explorer"}, {"task manager", "taskmgr", "Task Manager"},
        {"command prompt", "cmd", "Command Prompt"}, {"terminal", "cmd", "Command Prompt"},
        {"settings", "ms-settings:", nullptr},      {"browser", "https://www.google.com", nullptr},
        {"chrome", "chrome", nullptr},             {"edge", "msedge", nullptr},
    };

    // People write "open notepad and type hello then press enter": split on the
    // connectives and plan each clause on its own.
    std::string norm = goal;
    for (const char* sep : {" then ", " and then ", " and ", ",", ";", "\n"}) {
        size_t p = 0, len = strlen(sep);
        while ((p = norm.find(sep, p)) != std::string::npos) norm.replace(p, len, "|");
    }
    std::vector<std::string> clauses;
    for (size_t pos = 0; pos <= norm.size();) {
        size_t bar = norm.find('|', pos);
        std::string c = trim(norm.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
        if (!c.empty()) clauses.push_back(c);
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }

    std::vector<std::string> cmds;
    Rect v = virtual_desktop();
    for (const std::string& clause : clauses) {
        std::string low = to_lower(clause);
        auto after = [&](size_t n) { return trim(clause.substr(n)); };
        auto begins = [&](const char* p) { return starts_with(low, p); };

        if (begins("screenshot") || begins("take a screenshot") || begins("capture the screen") ||
            begins("capture screen")) {
            cmds.push_back("shot");
            continue;
        }
        for (const char* pre : {"search for ", "search the web for ", "google ", "look up "}) {
            if (begins(pre)) {
                cmds.push_back("open https://www.google.com/search?q=" + url_encode(after(strlen(pre))));
                break;
            }
        }
        if (!cmds.empty() && starts_with(cmds.back(), "open https://www.google.com/search")) continue;

        if (begins("create ") || begins("make ")) {
            std::string lowc = low;
            bool folder = lowc.find("folder") != std::string::npos ||
                          lowc.find("directory") != std::string::npos;
            size_t named = lowc.find("named ");
            size_t called = lowc.find("called ");
            size_t at = named != std::string::npos ? named : called;
            if (at != std::string::npos) {
                std::string rest = after(at + 7);
                std::string name = rest, body;
                for (const char* sep : {" with ", " containing ", " that says "}) {
                    size_t sp = to_lower(rest).find(sep);
                    if (sp != std::string::npos) {
                        name = trim(rest.substr(0, sp));
                        body = trim(rest.substr(sp + strlen(sep)));
                        break;
                    }
                }
                name = trim(name.substr(0, name.find_first_of(".,")));
                if (!name.empty()) {
                    cmds.push_back((folder ? "mkdir " : "write ") + name + (folder ? "" : " " + body));
                    continue;
                }
            }
        }

        if (begins("open ") || begins("launch ") || begins("start ") || begins("run ")) {
            std::string rest = after(low.find(' ') + 1);
            std::string low_rest = to_lower(rest);
            if (low_rest.find("http") != std::string::npos || starts_with(low_rest, "www.") ||
                low_rest.find(".com") != std::string::npos || low_rest.find(".org") != std::string::npos ||
                low_rest.find(".net") != std::string::npos) {
                cmds.push_back("open " + rest);
                continue;
            }
            const AppAlias* hit = nullptr;
            for (const AppAlias& a : apps)
                if (low_rest.find(a.alias) != std::string::npos) {
                    hit = &a;
                    break;
                }
            if (hit) {
                cmds.push_back(std::string("open ") + hit->command);
                cmds.push_back("wait 2000");
                if (hit->focus_hint) cmds.push_back(std::string("focus ") + hit->focus_hint);
                continue;
            }
            cmds.push_back("open " + rest);
            cmds.push_back("wait 2000");
            cmds.push_back("focus " + rest);
            continue;
        }

        if (begins("close ")) {
            cmds.push_back("close " + after(6));
            continue;
        }
        if (begins("type ") || begins("write ") || begins("enter ") || begins("say ")) {
            cmds.push_back("type " + after(low.find(' ') + 1));
            continue;
        }
        if (begins("press ") || begins("hit ") || begins("hold ")) {
            std::string k = after(low.find(' ') + 1);
            size_t then_i = k.find_first_of('.');
            if (then_i != std::string::npos) k = trim(k.substr(0, then_i));
            cmds.push_back("key " + k);
            continue;
        }
        if (begins("click ")) {
            std::string rest = after(6);
            std::vector<std::string> nums;
            std::string cur;
            for (char c : rest) {
                if (isdigit((unsigned char)c)) cur.push_back(c);
                else if (!cur.empty()) {
                    nums.push_back(cur);
                    cur.clear();
                }
            }
            if (!cur.empty()) nums.push_back(cur);
            if (nums.size() >= 2)
                cmds.push_back("click left " + nums[0] + " " + nums[1]);
            else
                cmds.push_back("click left " + std::to_string(v.x + v.w / 2) + " " +
                               std::to_string(v.y + v.h / 2));
            continue;
        }
        if (begins("scroll ")) {
            int n = 3;
            std::string rest = to_lower(clause);
            for (size_t i = 0; i < rest.size(); i++)
                if (isdigit((unsigned char)rest[i])) { n = atoi(rest.c_str() + i); break; }
            if (low.find("up") != std::string::npos) n = -n;
            cmds.push_back("scroll " + std::to_string(n));
            continue;
        }
        if (begins("move ")) {
            std::string rest = after(5);
            std::vector<std::string> nums;
            std::string cur;
            for (char c : rest) {
                if (isdigit((unsigned char)c)) cur.push_back(c);
                else if (!cur.empty()) { nums.push_back(cur); cur.clear(); }
            }
            if (!cur.empty()) nums.push_back(cur);
            if (nums.size() >= 2) cmds.push_back("move " + nums[0] + " " + nums[1]);
            continue;
        }
        if (low.find("what") != std::string::npos && low.find("window") != std::string::npos) {
            cmds.push_back("windows 15");
            continue;
        }
        if (begins("list ") && low.find("window") != std::string::npos) {
            cmds.push_back("windows 15");
            continue;
        }
        if (begins("wait ")) {
            int n = 500;
            for (size_t i = 0; i < low.size(); i++)
                if (isdigit((unsigned char)low[i])) { n = atoi(low.c_str() + i); break; }
            if (low.find("second") != std::string::npos) n *= 1000;
            cmds.push_back("wait " + std::to_string(n));
            continue;
        }
        if (begins("focus ") || begins("switch to ") || begins("bring ")) {
            size_t sp = low.find(' ');
            std::string rest = after(sp + 1);
            for (const char* pre : {"to ", "up "}) 
                if (starts_with(to_lower(rest), pre)) rest = trim(rest.substr(strlen(pre)));
            cmds.push_back("focus " + rest);
            continue;
        }
    }
    return cmds;
}

static std::string join_commands(const std::vector<std::string>& cmds) {
    std::string script;
    for (const std::string& c : cmds) script += c + "\n";
    return script;
}

bool Agent::run_goal(const std::string& goal) {
    if (goal.empty()) {
        log("fail", "no goal given");
        return false;
    }
    if (config.api_key.empty()) {
        // No model, but the built-in planner can still do a great deal.
        std::vector<std::string> local = plan_locally(goal);
        if (!local.empty()) {
            log("info", "no model key set: using the built-in planner (" +
                             std::to_string(local.size()) + " step(s))");
            for (const std::string& c : local) log("agent", "planned: " + c);
            std::string script = join_commands(local);
            publish_script(script);
            return run_script(script);
        }
        // A goal that is already a script still works verbatim.
        std::vector<std::string> words = split_words(goal);
        bool looks_like_script = goal.find('\n') != std::string::npos || (!words.empty() &&
            (to_lower(words[0]) == "shot" || to_lower(words[0]) == "click" ||
             to_lower(words[0]) == "move" || to_lower(words[0]) == "type" ||
             to_lower(words[0]) == "key" || to_lower(words[0]) == "focus"));
        if (looks_like_script) {
            log("info", "no model key: running the goal as a command script");
            return run_script(goal);
        }
        log("warn", "no model key set, so Hawk OS cannot plan from a screenshot. Press Model to add "
                    "one, or try a plain instruction like \"open notepad and type hello\".");
        return false;
    }

    request_abort(false);
    save_cursor_origin();
    std::string transcript;
    std::string generated;
    bool done = false;

    for (int round = 0; round < config.max_rounds && !done; round++) {
        if (abort_requested()) {
            log("warn", "goal aborted");
            break;
        }
        std::string shot;
        std::string b64;
        if (!config.dry_run) {
            Image full = capture_screen();
            if (full.empty()) {
                log("fail", "could not capture the screen");
                break;
            }
            Image small = scale_to_fit(full, config.vision ? 1280 : 1024, config.vision ? 800 : 640);
            Point c = cursor_pos();
            Rect v = virtual_desktop();
            Image marked = draw_marker(small, (c.x - v.x) * small.w / std::max(1, full.w),
                                       (c.y - v.y) * small.h / std::max(1, full.h));
            shot = take_shot("goal");
            save_png(small, shot);  // the file the human can open afterwards
            last_shot_ = shot;
            std::vector<uint8_t> png;
            // reuse the encoder to get PNG bytes for the model
            std::string tmp = shot_dir() + "\\_model.png";
            if (save_png(marked, tmp)) {
                FILE* f = fopen(tmp.c_str(), "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long n = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    png.resize((size_t)(n > 0 ? n : 0));
                    if (!png.empty()) fread(png.data(), 1, png.size(), f);
                    fclose(f);
                }
                DeleteFileA(tmp.c_str());
            }
            if (config.vision && !png.empty()) b64 = b64_encode(png);
        }

        std::string prompt;
        prompt += "You are the driver of a real Windows desktop. You act by replying with commands "
                  "only, one per line, in the command language below. Nothing else: no prose, no "
                  "code fences.\n\nGoal: " + goal + "\n\n";
        prompt += command_reference();
        prompt += "\nRules:\n"
                  "- Use absolute pixel coordinates for this screen.\n"
                  "- Prefer clicking on things you can see in the screenshot.\n"
                  "- After acting, verify with `shot` or `verifypixel` when useful.\n"
                  "- If the goal is already satisfied, reply with exactly: done\n"
                  "- Reply with at most 8 commands.\n\n";
        prompt += screen_summary();
        if (!transcript.empty()) prompt += "\nWhat you already did:\n" + transcript;
        if (!b64.empty())
            prompt += "\nThe attached image is the current screen, downscaled. A green crosshair "
                      "marks the current mouse position.\n";

        std::string reply, err;
        log("agent", "planning round " + std::to_string(round + 1) + " with " + config.model +
                         (b64.empty() ? " (text only)" : " (with screenshot)"));
        if (!llm_chat(prompt, b64, &reply, &err)) {
            log("fail", "model call failed: " + err);
            std::vector<std::string> local = plan_locally(goal);
            if (!local.empty() && round == 0) {
                log("warn", "acting without the model: the built-in planner understood " +
                                std::to_string(local.size()) + " step(s)");
                for (const std::string& c : local) log("agent", "planned: " + c);
                std::string script = join_commands(local);
                publish_script(script);
                run_script(script);
            } else if (local.empty()) {
                log("info", "check the base URL, model id and key with the Model button "
                            "(the endpoint must be OpenAI-compatible, e.g. .../v1)");
            }
            break;
        }
        log("agent", "model said: " + reply.substr(0, 400));

        std::vector<std::string> cmds = parse_commands(reply);
        if (cmds.empty()) {
            log("warn", "model returned no usable commands; stopping");
            break;
        }
        generated += join_commands(cmds);
        publish_script(generated);  // visible in the script box, and on disk
        if (!config.auto_run) {
            log("warn", "auto-run is off, so the generated script was saved for review and "
                        "nothing was executed");
            break;
        }
        int ran = 0;
        for (const std::string& c : cmds) {
            if (to_lower(c) == "done") {
                done = true;
                break;
            }
            if (ran++ >= config.max_steps) break;
            if (abort_requested()) break;
            if (config.step_delay_ms > 0) sleep_ms(config.step_delay_ms);
            bool ok = step(c, 0);
            transcript += (ok ? "  ok   " : "  FAIL ") + c + "\n";
            log(ok ? "ok" : "fail", "[model] " + c);
        }
        if (done) log("ok", "model reports the goal is complete");
    }

    restore_cursor_origin();
    log(done ? "ok" : "warn", done ? "goal finished" : "goal stopped without a completion signal");
    return done;
}

}  // namespace hawk
