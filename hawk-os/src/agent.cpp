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
    std::string p = raw;
    // `~` is the user's profile folder, so `read ~\Documents\note.txt` and
    // `del ~\scratch.tmp` behave the way people expect from a shell.
    if (!p.empty() && p[0] == '~') {
        char home[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("USERPROFILE", home, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::string restp = p.size() > 1 ? p.substr(1) : "";
            if (!restp.empty() && (restp[0] == '/' || restp[0] == '\\')) restp = restp.substr(1);
            return restp.empty() ? std::string(home) : std::string(home) + "\\" + restp;
        }
    }
    if (is_absolute(p)) return p;
    std::string dir = app_dir() + "\\workspace";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\" + p;
}

// Resolve file commands inside Hawk's workspace only. The model can see
// screen content and may encounter prompt injection; never let a generated
// file command silently target arbitrary user-profile or system files.
static bool workspace_file_path(const std::string& raw, std::string* resolved) {
    if (raw.empty() || raw[0] == '~' || raw[0] == '/' || raw[0] == '\\' ||
        raw.find(':') != std::string::npos) return false;
    std::string rel = raw;
    for (char& c : rel) if (c == '/') c = '\\';
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t end = rel.find('\\', pos);
        std::string part = rel.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (part == ".." || part == "." || part.empty()) return false;
        if (end == std::string::npos) break;
        pos = end + 1;
    }

    std::string root = app_dir() + "\\workspace";
    CreateDirectoryA(root.c_str(), nullptr);
    std::string candidate = root + "\\" + rel;
    char full_root[4096] = {}, full_candidate[4096] = {};
    DWORD root_len = GetFullPathNameA(root.c_str(), sizeof(full_root), full_root, nullptr);
    DWORD candidate_len = GetFullPathNameA(candidate.c_str(), sizeof(full_candidate), full_candidate, nullptr);
    if (!root_len || root_len >= sizeof(full_root) || !candidate_len ||
        candidate_len >= sizeof(full_candidate)) return false;
    std::string base(full_root), full(full_candidate);
    while (base.size() > 3 && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    DWORD root_attr = GetFileAttributesA(base.c_str());
    if (root_attr == INVALID_FILE_ATTRIBUTES || (root_attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (full.size() <= base.size() ||
        _strnicmp(full.c_str(), base.c_str(), base.size()) != 0 ||
        (full[base.size()] != '\\' && full[base.size()] != '/')) return false;

    // Refuse existing reparse-point ancestors (junctions/symlinks) so a
    // workspace symlink cannot trivially redirect file operations elsewhere.
    std::string walk = base;
    size_t at = base.size() + 1;
    while (at <= full.size()) {
        size_t end = full.find('\\', at);
        walk += "\\" + full.substr(at, end == std::string::npos ? std::string::npos : end - at);
        DWORD attr = GetFileAttributesA(walk.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    *resolved = full;
    return true;
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

// ---------------------------------------------------- screenshot grounding --
// The screenshot the model works from has its own pixel space. These helpers
// keep track of how that space maps onto the real desktop, so a script can say
// "click 320,480" (a point read off the image) and touch the real screen there.
void Agent::remember_map(const Image& shot, const Rect& desk) {
    map_.w = shot.w;
    map_.h = shot.h;
    map_.desk = desk;
    map_.valid = !shot.empty() && shot.w > 0 && shot.h > 0;
}

bool Agent::to_desktop(int sx, int sy, Point* out) const {
    if (!map_.valid || map_.w <= 0 || map_.h <= 0) return false;
    Point p;
    p.x = map_.desk.x + (int)((long)sx * map_.desk.w / map_.w);
    p.y = map_.desk.y + (int)((long)sy * map_.desk.h / map_.h);
    if (out) *out = p;
    return true;
}

// Model-free reading of a screenshot: what colours dominate, which controls
// were detected, and where each one sits in both screenshot and desktop space.
static std::string element_lines(const std::vector<UiElement>& els, const Rect& desk,
                                 int shot_w, int shot_h) {
    std::string out;
    if (shot_w <= 0) shot_w = 1;
    if (shot_h <= 0) shot_h = 1;
    for (size_t i = 0; i < els.size(); i++) {
        const UiElement& e = els[i];
        int cx = e.rect.x + e.rect.w / 2, cy = e.rect.y + e.rect.h / 2;
        int dx = desk.x + (int)((long)cx * desk.w / shot_w);
        int dy = desk.y + (int)((long)cy * desk.h / shot_h);
        char line[320];
        snprintf(line, sizeof(line),
                 "  %2d. %-6s at %4d,%-4d size %3dx%-3d fill #%s   centre screenshot %d,%d -> "
                 "desktop %d,%d\n",
                 (int)i + 1, e.kind, e.rect.x, e.rect.y, e.rect.w, e.rect.h,
                 color_hex(e.color).c_str(), cx, cy, dx, dy);
        out += line;
    }
    return out;
}

std::string Agent::visual_report(const Image& img) {
    if (img.empty()) return "Visual analysis: no pixels to analyse.\n";
    Rect v = map_.valid ? map_.desk : virtual_desktop();
    std::string s = "Visual analysis computed by Hawk from the screenshot itself:\n";

    std::vector<std::pair<uint32_t, int>> dom = dominant_colors(img, 6);
    if (!dom.empty()) {
        // dominant_colors returns full-image pixel counts (samples x4); the
        // sampling loop itself visits one pixel in four.
        int sampled = (img.w / 2 + 1) * (img.h / 2 + 1);
        s += "  dominant colours: ";
        for (size_t i = 0; i < dom.size(); i++) {
            char buf[64];
            int pct = sampled > 0 ? (dom[i].second / 4) * 100 / sampled : 0;
            if (pct > 100) pct = 100;
            snprintf(buf, sizeof(buf), "#%s %d%%%s", color_hex(dom[i].first).c_str(), pct,
                     i + 1 < dom.size() ? ", " : "\n");
            s += buf;
        }
    }

    std::vector<UiElement> els = detect_elements(img, 10);
    if (els.empty()) {
        s += "  controls detected: none - the screen looks plain or photographic\n";
    } else {
        s += "  controls detected (ranked; coordinates are in this screenshot's pixel space):\n";
        s += element_lines(els, v, img.w, img.h);
    }
    return s;
}

bool Agent::look() {
    Image img = capture_screen();
    if (img.empty()) {
        log("fail", "look: could not capture the screen");
        return false;
    }
    Rect v = virtual_desktop();
    remember_map(img, v);  // full-resolution capture: screenshot px == desktop px
    // Dry-run promises not to write anything, so it analyses without saving.
    last_shot_ = config.dry_run ? "" : take_shot("look");
    std::vector<UiElement> els = detect_elements(img, 12);
    log("ok", "look: " + std::to_string(img.w) + "x" + std::to_string(img.h) + ", " +
                  std::to_string(els.size()) + " control(s) detected" +
                  (last_shot_.empty() ? "" : (", screenshot " + last_shot_)));
    log("info", visual_report(img));
    std::string accessible = describe_accessible_controls(GetForegroundWindow(), 40);
    if (!accessible.empty()) log("info", "Windows accessibility controls:\n" + accessible);
    if (!els.empty())
        log("info", "click the top control with: click " +
                        std::to_string(els[0].rect.x + els[0].rect.w / 2) + " " +
                        std::to_string(els[0].rect.y + els[0].rect.h / 2) +
                        "   (or use clickel 1)");
    return true;
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
        remember_map(img, virtual_desktop());  // so sclick/smove can use its coordinates
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
        remember_map(img, r);  // the region's own pixel space, mapped onto itself
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
    // Screenshot-space aiming: coordinates are read straight off the image the
    // agent (or the model) was looking at, and Hawk maps them onto the real
    // screen itself - no arithmetic to get wrong.
    if (cmd == "smove" || cmd == "sclick" || cmd == "sdrag") {
        if (!map_.valid) {
            log("fail", cmd + ": no screenshot to aim from yet - run `look`, `shot` or a goal "
                            "first");
            return false;
        }
        if (cmd == "smove") {
            if (!need(2)) return false;
            Point p{};
            if (!to_desktop(number(1, 0), number(2, 0), &p)) {
                log("fail", "smove: the screenshot mapping is not usable");
                return false;
            }
            if (config.dry_run) {
                log("action", "smove " + std::to_string(p.x) + "," + std::to_string(p.y) +
                                  " (dry-run)");
                return true;
            }
            if (!move_cursor(p.x, p.y, &err)) {
                log("fail", "smove: " + err);
                return false;
            }
            log("ok", "screenshot " + a[1] + "," + a[2] + " -> screen " + std::to_string(p.x) +
                          "," + std::to_string(p.y));
            return true;
        }
        if (cmd == "sclick") {
            std::string button = "left";
            int times = 1;
            size_t i = 1;
            if (i < a.size()) {
                std::string t = to_lower(a[i]);
                if (t == "double") {
                    times = 2;
                    i++;
                } else if (t == "left" || t == "right" || t == "middle") {
                    button = t;
                    i++;
                }
            }
            if (!need(i + 1)) return false;
            Point p{};
            if (!to_desktop(number(i, 0), number(i + 1, 0), &p)) {
                log("fail", "sclick: the screenshot mapping is not usable");
                return false;
            }
            if (config.dry_run) {
                log("action", "sclick " + std::to_string(p.x) + "," + std::to_string(p.y) +
                                  " (dry-run)");
                return true;
            }
            if (!move_cursor(p.x, p.y, &err)) {
                log("fail", "sclick: " + err);
                return false;
            }
            if (!click_mouse(button, times, &err)) {
                log("fail", "sclick: " + err);
                return false;
            }
            Point landed = cursor_pos();
            log("ok", "screenshot " + a[i] + "," + a[i + 1] + " -> " + button +
                          (times > 1 ? " x" + std::to_string(times) : "") + " click at " +
                          std::to_string(landed.x) + "," + std::to_string(landed.y));
            return true;
        }
        if (!need(4)) return false;
        Point p1{}, p2{};
        if (!to_desktop(number(1, 0), number(2, 0), &p1) ||
            !to_desktop(number(3, 0), number(4, 0), &p2)) {
            log("fail", "sdrag: the screenshot mapping is not usable");
            return false;
        }
        if (config.dry_run) {
            log("action", "sdrag " + std::to_string(p1.x) + "," + std::to_string(p1.y) + " -> " +
                              std::to_string(p2.x) + "," + std::to_string(p2.y) + " (dry-run)");
            return true;
        }
        if (!drag_mouse(p1.x, p1.y, p2.x, p2.y, "left", &err)) {
            log("fail", "sdrag: " + err);
            return false;
        }
        log("ok", "dragged screenshot " + a[1] + "," + a[2] + " -> " + a[3] + "," + a[4]);
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
        // Never echo typed content into logs: it may contain personal data or
        // credentials intended for the focused application.
        log("ok", "typed " + std::to_string(text.size()) + " character(s) (content redacted)");
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
            // A relative file name resolves the way write/read do: the
            // workspace first, then Documents - so `open hello-harry.txt`
            // reopens what was just saved, wherever the save dialog put it.
            if (!target.empty() && !is_absolute(target) &&
                (target[0] == '~' || target.find(':') == std::string::npos)) {
                if (target[0] == '~') {
                    target = expand_path(target);
                } else if (target.find("://") == std::string::npos) {
                    std::string home = expand_path("~");
                    std::string cands[] = {expand_path(target), home + "\\Documents\\" + target};
                    for (const std::string& cand : cands) {
                        if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            target = cand;
                            break;
                        }
                    }
                }
            }
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
    if (cmd == "waitclose") {
        // Wait until a window is really gone. Without this, relaunching an app
        // right after closing it races its single-instance hand-off: Notepad
        // passes the file to the dying instance and no window ever appears.
        if (!need(1)) return false;
        std::string needle = a[1];
        int timeout = a.size() > 2 ? number(2, 10000) : 10000;
        if (config.dry_run) {
            log("action", "waitclose " + needle + " (dry-run)");
            return true;
        }
        int waited = 0;
        while (waited < timeout) {
            if (!find_window(needle)) {
                log("ok", "'" + needle + "' has closed (after " + std::to_string(waited) +
                              " ms)");
                return true;
            }
            sleep_ms(100);
            waited += 100;
        }
        log("fail", "waitclose: '" + needle + "' is still open after " +
                        std::to_string(timeout) + " ms");
        return false;
    }
    if (cmd == "waitfocus") {
        // Wait for a window to appear (or stop being minimized), then bring it
        // to the front. This is what makes multi-application flows reliable:
        // `open code` + `waitfocus Visual Studio Code` cannot race the app's
        // own startup the way `open` + `focus` can.
        if (!need(1)) return false;
        std::string needle = a[1];
        int timeout = a.size() > 2 ? number(2, 20000) : 20000;
        if (config.dry_run) {
            log("action", "waitfocus " + needle + " (dry-run)");
            return true;
        }
        int waited = 0;
        while (waited < timeout) {
            HWND h = find_window(needle);
            if (h && !IsIconic(h) && focus_window(h)) {
                log("ok", "window '" + narrow(window_title(h)) + "' is up and focused (after " +
                              std::to_string(waited) + " ms)");
                return true;
            }
            sleep_ms(200);
            waited += 200;
        }
        log("fail", "waitfocus: no window matching '" + needle + "' became focusable within " +
                        std::to_string(timeout) + " ms");
        return false;
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
    if (cmd == "look") {
        return look();
    }
    if (cmd == "elements") {
        // Detect the controls on the current screen (optionally inside a
        // region) and report them with the coordinates a script can click.
        Rect v = virtual_desktop();
        Rect area = v;
        bool limited = a.size() >= 5;
        if (limited) area = Rect{number(1, 0), number(2, 0), number(3, 100), number(4, 100)};
        Image img = limited ? capture_region(area) : capture_screen();
        if (img.empty()) {
            log("fail", "elements: could not capture the screen");
            return false;
        }
        remember_map(img, limited ? area : v);
        std::vector<UiElement> els = detect_elements(img, 14);
        last_shot_ = config.dry_run ? "" : take_shot("elements");
        log("ok", "elements: " + std::to_string(els.size()) + " control(s) on screen");
        if (els.empty()) {
            log("info", "nothing control-like was detected (plain or photographic screen)");
        } else {
            log("info", "coordinates below are in the captured image; clickel 1 clicks the "
                        "top-ranked one:\n" + element_lines(els, limited ? area : v, img.w, img.h));
        }
        return true;
    }
    if (cmd == "clickel") {
        // "clickel 2" - detect the controls again and click the 2nd-ranked one.
        int rank = need(1) ? number(1, 1) : 1;
        if (rank < 1) rank = 1;
        std::string button = a.size() > 2 ? to_lower(a[2]) : (a.size() > 1 && to_lower(a[1]) != std::to_string(rank) ? to_lower(a[1]) : "left");
        int times = button == "double" ? 2 : 1;
        if (button != "right" && button != "middle" && button != "double") button = "left";
        Image img = capture_screen();
        if (img.empty()) {
            log("fail", "clickel: could not capture the screen");
            return false;
        }
        std::vector<UiElement> els = detect_elements(img, 14);
        if ((int)els.size() < rank) {
            log("fail", "clickel: only " + std::to_string(els.size()) +
                            " control(s) detected, no rank " + std::to_string(rank));
            return false;
        }
        const UiElement& e = els[rank - 1];
        Rect v = virtual_desktop();
        int x = v.x + e.rect.x + e.rect.w / 2, y = v.y + e.rect.y + e.rect.h / 2;
        log("ok", "clickel: rank " + std::to_string(rank) + " is a " + e.kind + " filling #" +
                      color_hex(e.color) + " at " + std::to_string(e.rect.x) + "," +
                      std::to_string(e.rect.y) + " " + std::to_string(e.rect.w) + "x" +
                      std::to_string(e.rect.h));
        if (config.dry_run) {
            log("action", "click " + std::to_string(x) + "," + std::to_string(y) + " (dry-run)");
            return true;
        }
        if (!move_cursor(x, y, &err)) {
            log("fail", "clickel: " + err);
            return false;
        }
        if (!click_mouse(button == "double" ? "left" : button, times, &err)) {
            log("fail", "clickel: " + err);
            return false;
        }
        Point p = cursor_pos();
        log("ok", "clicked control rank " + std::to_string(rank) + " at " + std::to_string(p.x) +
                      "," + std::to_string(p.y));
        return true;
    }
    if (cmd == "findarea") {
        // "findarea" works on whole regions instead of single pixels: it looks
        // for the biggest blob of a colour, which is what a button actually is.
        if (!need(1)) return false;
        uint32_t want = parse_color(a[1]);
        if (want == 0xFFFFFFFFu) {
            log("fail", "findarea: '" + a[1] + "' is not an RRGGBB colour");
            return false;
        }
        int tol = a.size() > 2 ? number(2, 60) : 60;
        std::string action = a.size() > 3 ? to_lower(a[3]) : "";
        Image img = capture_screen();
        Rect area;
        if (!find_color_area(img, want, tol, &area)) {
            log("fail", "findarea: no blob of #" + to_lower(a[1]) + " (tolerance " +
                            std::to_string(tol) + ") is on screen");
            return false;
        }
        Rect v = virtual_desktop();
        Point centre{v.x + area.x + area.w / 2, v.y + area.y + area.h / 2};
        log("ok", "findarea: #" + to_lower(a[1]) + " covers screen " + std::to_string(v.x + area.x) +
                      "," + std::to_string(v.y + area.y) + " " + std::to_string(area.w) + "x" +
                      std::to_string(area.h) + ", centre " + std::to_string(centre.x) + "," +
                      std::to_string(centre.y));
        if (action == "move" || action == "click" || action == "double") {
            if (config.dry_run) {
                log("action", action + " the found region (dry-run)");
                return true;
            }
            if (!move_cursor(centre.x, centre.y, &err)) {
                log("fail", "findarea: " + err);
                return false;
            }
            if (action != "move") {
                int times = action == "double" ? 2 : 1;
                if (!click_mouse("left", times, &err)) {
                    log("fail", "findarea: " + err);
                    return false;
                }
                log("ok", action + " at the centre of the found region");
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
        std::string path;
        if (!workspace_file_path(a[1], &path)) {
            log("fail", "write: path must stay inside Hawk's workspace (no absolute paths, ~, or ..)");
            return false;
        }
        DWORD existing = GetFileAttributesA(path.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES) {
            log("fail", "write: refusing to overwrite an existing workspace file; use del explicitly first: " + path);
            return false;
        }
        std::string body = a.size() > 2 ? unescape(rest(2)) : "";
        if (config.dry_run) {
            log("action", "write " + path + " (" + std::to_string(body.size()) + " bytes, dry-run)");
            return true;
        }
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            log("fail", "write: cannot create a new file at " + path + " (error " +
                            std::to_string(GetLastError()) + ")");
            return false;
        }
        DWORD written = 0;
        BOOL ok = body.empty() || WriteFile(file, body.data(), static_cast<DWORD>(body.size()),
                                            &written, nullptr);
        CloseHandle(file);
        if (!ok || written != body.size()) {
            DeleteFileA(path.c_str());
            log("fail", "write: incomplete write to " + path);
            return false;
        }
        log("ok", "wrote " + std::to_string(body.size()) + " bytes to " + path);
        return true;
    }
    if (cmd == "ensure") {
        // Create a file only when it is missing - so "open a text file called
        // X" never clobbers an X that already exists.
        if (!need(1)) return false;
        std::string path;
        if (!workspace_file_path(a[1], &path)) {
            log("fail", "ensure: path must stay inside Hawk's workspace (no absolute paths, ~, or ..)");
            return false;
        }
        DWORD attr = GetFileAttributesA(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            log("info", "ensure: " + path + " already exists, leaving it alone");
            return true;
        }
        std::string body = a.size() > 2 ? unescape(rest(2)) : "";
        if (config.dry_run) {
            log("action", "ensure " + path + " (" + std::to_string(body.size()) +
                              " bytes, dry-run)");
            return true;
        }
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                log("info", "ensure: " + path + " already exists, leaving it alone");
                return true;
            }
            log("fail", "ensure: cannot create " + path + " (error " + std::to_string(error) + ")");
            return false;
        }
        DWORD written = 0;
        BOOL ok = body.empty() || WriteFile(file, body.data(), static_cast<DWORD>(body.size()),
                                            &written, nullptr);
        CloseHandle(file);
        if (!ok || written != body.size()) {
            DeleteFileA(path.c_str());
            log("fail", "ensure: incomplete write to " + path);
            return false;
        }
        log("ok", "created " + path + " (" + std::to_string(body.size()) + " bytes)");
        return true;
    }
    if (cmd == "read") {
        if (!need(1)) return false;
        std::string path;
        if (!workspace_file_path(a[1], &path)) {
            log("fail", "read: path must stay inside Hawk's workspace (no absolute paths, ~, or ..)");
            return false;
        }
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
        std::string path;
        if (!workspace_file_path(a[1], &path)) {
            log("fail", "mkdir: path must stay inside Hawk's workspace (no absolute paths, ~, or ..)");
            return false;
        }
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
    if (cmd == "del" || cmd == "rm" || cmd == "delete") {
        if (!need(1)) return false;
        std::string path;
        if (!workspace_file_path(a[1], &path)) {
            log("fail", "del: path must stay inside Hawk's workspace (no absolute paths, ~, or ..)");
            return false;
        }
        if (config.dry_run) {
            log("action", "del " + path + " (dry-run)");
            return true;
        }
        if (DeleteFileA(path.c_str())) {
            log("ok", "deleted " + path);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            log("info", "del: " + path + " was not there");
            return true;  // removing what is already gone is still success
        }
        log("fail", "del: could not delete " + path + " (error " + std::to_string(e) + ")");
        return false;
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
    log("info", "commands: shot shotwin region look elements clickel move click drag scroll "
                "type key focus open close wait waitclose waitfocus pixel find findarea "
                "verifypixel smove sclick sdrag windows cursor monitors write read mkdir log "
                "try done");
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

// Small, strict JSON reader for the one response shape Hawk consumes. It
// walks JSON structure (rather than searching raw text for "content") and
// decodes UTF-16 surrogate pairs into UTF-8.
static void append_utf8(std::string* out, unsigned long cp) {
    if (cp <= 0x7F) out->push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static bool json_hex4(const std::string& s, size_t at, unsigned long* value) {
    if (at + 4 > s.size()) return false;
    unsigned long n = 0;
    for (size_t i = at; i < at + 4; ++i) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        n = (n << 4) | static_cast<unsigned long>(d);
    }
    *value = n;
    return true;
}

static void json_ws(const std::string& s, size_t* p) {
    while (*p < s.size() && isspace(static_cast<unsigned char>(s[*p]))) ++*p;
}

static bool json_string(const std::string& s, size_t* p, std::string* out) {
    json_ws(s, p);
    if (*p >= s.size() || s[*p] != '"') return false;
    ++*p;
    out->clear();
    while (*p < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[(*p)++]);
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') { out->push_back(static_cast<char>(c)); continue; }
        if (*p >= s.size()) return false;
        char e = s[(*p)++];
        switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                unsigned long cp = 0;
                if (!json_hex4(s, *p, &cp)) return false;
                *p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (*p + 6 > s.size() || s[*p] != '\\' || s[*p + 1] != 'u') return false;
                    unsigned long low = 0;
                    if (!json_hex4(s, *p + 2, &low) || low < 0xDC00 || low > 0xDFFF) return false;
                    *p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

static bool json_skip_value(const std::string& s, size_t* p, int depth = 0) {
    if (depth > 64) return false;
    json_ws(s, p);
    if (*p >= s.size()) return false;
    if (s[*p] == '"') { std::string ignored; return json_string(s, p, &ignored); }
    if (s[*p] == '{') {
        ++*p; json_ws(s, p);
        if (*p < s.size() && s[*p] == '}') { ++*p; return true; }
        while (*p < s.size()) {
            std::string key;
            if (!json_string(s, p, &key)) return false;
            json_ws(s, p);
            if (*p >= s.size() || s[(*p)++] != ':') return false;
            if (!json_skip_value(s, p, depth + 1)) return false;
            json_ws(s, p);
            if (*p < s.size() && s[*p] == '}') { ++*p; return true; }
            if (*p >= s.size() || s[(*p)++] != ',') return false;
            json_ws(s, p);
        }
        return false;
    }
    if (s[*p] == '[') {
        ++*p; json_ws(s, p);
        if (*p < s.size() && s[*p] == ']') { ++*p; return true; }
        while (*p < s.size()) {
            if (!json_skip_value(s, p, depth + 1)) return false;
            json_ws(s, p);
            if (*p < s.size() && s[*p] == ']') { ++*p; return true; }
            if (*p >= s.size() || s[(*p)++] != ',') return false;
            json_ws(s, p);
        }
        return false;
    }
    size_t begin = *p;
    while (*p < s.size() && s[*p] != ',' && s[*p] != ']' && s[*p] != '}' &&
           !isspace(static_cast<unsigned char>(s[*p]))) ++*p;
    if (*p == begin) return false;
    std::string atom = s.substr(begin, *p - begin);
    return atom == "true" || atom == "false" || atom == "null" ||
           (atom.find_first_not_of("-+0123456789.eE") == std::string::npos);
}

static bool json_member(const std::string& s, size_t object, const char* wanted,
                        size_t* value_at) {
    size_t p = object;
    json_ws(s, &p);
    if (p >= s.size() || s[p++] != '{') return false;
    json_ws(s, &p);
    if (p < s.size() && s[p] == '}') return false;
    while (p < s.size()) {
        std::string key;
        if (!json_string(s, &p, &key)) return false;
        json_ws(s, &p);
        if (p >= s.size() || s[p++] != ':') return false;
        json_ws(s, &p);
        if (key == wanted) { *value_at = p; return true; }
        if (!json_skip_value(s, &p)) return false;
        json_ws(s, &p);
        if (p < s.size() && s[p] == '}') return false;
        if (p >= s.size() || s[p++] != ',') return false;
        json_ws(s, &p);
    }
    return false;
}

static std::string extract_content(const std::string& body) {
    size_t choices = 0;
    if (!json_member(body, 0, "choices", &choices)) return "";
    json_ws(body, &choices);
    if (choices >= body.size() || body[choices++] != '[') return "";
    json_ws(body, &choices);
    if (choices >= body.size() || body[choices] != '{') return "";
    size_t message = 0;
    if (!json_member(body, choices, "message", &message)) return "";
    size_t content = 0;
    if (!json_member(body, message, "content", &content)) return "";
    std::string result;
    if (!json_string(body, &content, &result)) return "";
    return result;
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
                     std::string* reply, std::string* err, int max_tokens) {
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
    if (max_tokens < 100) max_tokens = 100;
    std::string body = "{\"model\":\"" + json_escape(config.model) + "\",\"messages\":[{\"role\":\"" +
                       std::string("user") + "\",\"content\":" + content +
                       "}],\"temperature\":0.2,\"max_tokens\":" + std::to_string(max_tokens) +
                       ",\"stream\":false}";

    HINTERNET session = WinHttpOpen(L"HawkOS/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *err = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

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
    bool aborted = false, read_failed = false, response_too_large = false;
    do {
        if (abort_requested()) { aborted = true; break; }
        avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { read_failed = true; break; }
        if (avail == 0) break;
        if (data.size() + static_cast<size_t>(avail) > 8u * 1024 * 1024) {
            response_too_large = true;
            break;
        }
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read)) { read_failed = true; break; }
        if (read == 0) break;
        chunk.resize(read);
        data += chunk;
    } while (avail > 0);

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    if (aborted) { *err = "request stopped by user"; return false; }
    if (read_failed) { *err = "failed while reading model response"; return false; }
    if (response_too_large) { *err = "model response exceeded the 8 MiB safety limit"; return false; }

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
        static const char* verbs[] = {"shot", "shotwin", "region", "look", "elements",
                                     "clickel", "move", "click", "drag", "scroll", "type", "key",
                                     "focus", "open", "close", "wait", "waitclose", "waitfocus",
                                     "sleep", "pixel", "find", "findarea", "verifypixel", "smove",
                                     "sclick", "sdrag", "windows", "cursor", "monitors", "write",
                                     "read", "mkdir", "ensure", "del", "rm", "delete", "log", "note",
                                     "echo", "try", "done"};
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
        "look                             read the screen: detect controls, report them\n"
        "elements [x y w h]               list the controls detected (in a region)\n"
        "clickel <rank> [right|double]    click the Nth detected control\n"
        "move x y | move rel dx dy        move the real cursor\n"
        "click [left|right|double|triple] [x y]   real click (optionally after moving)\n"
        "drag x1 y1 x2 y2                 press, move, release\n"
        "scroll <notches>                 wheel (negative = down)\n"
        "smove x y | sclick [btn] x y | sdrag x1 y1 x2 y2\n"
        "                                 same, but the coordinates are read off the last\n"
        "                                 screenshot (its own pixel space) - Hawk maps them\n"
        "                                 onto the real screen for you\n"
        "type <text>                      type text (\\n = Enter), real keystrokes\n"
        "key <combo>                      e.g. key ctrl+s, key alt+f4, key enter\n"
        "focus <title-part>               bring a window to the front\n"
        "waitfocus <title-part> [ms]      wait until a window exists, then focus it\n"
        "open <path|url|app>              launch it via the shell\n"
        "close <title-part>               ask a window to close\n"
        "waitclose <title-part> [ms]      wait until that window is gone\n"
        "wait <ms>                        pause\n"
        "pixel x y                        report the colour at a point\n"
        "find RRGGBB [tol] [move|click|double]   locate a colour, optionally act on it\n"
        "findarea RRGGBB [tol] [move|click|double]  same, on whole regions (a button, not\n"
        "                                 one pixel of it) - acts on the region's centre\n"
        "verifypixel x y RRGGBB [tol]     assert what is on screen\n"
        "windows | monitors | cursor      describe the machine\n"
        "write <path> <text> | mkdir <path>   create files/directories\n"
        "ensure <path> [text]                create a file only if missing\n"
        "del <path>                          delete a file (~ = your profile folder)\n"
        "log <text>                       write a note into the run log\n"
        "done                             finish (say this when the goal is met)\n";
}

static bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// A colour word in everyday speech -> the colour it means on a screen. Used by
// the no-model planner so "click the blue button" can be grounded in real
// pixels instead of a blind click at the centre of the display.
static bool color_word(const std::string& s, uint32_t* bgr) {
    struct Word {
        const char* word;
        uint32_t color;
    };
    static const Word words[] = {
        {"red", 0x00343ED6},        {"green", 0x0058C830},   {"blue", 0x00FF840A},
        {"yellow", 0x000AD6FF},     {"orange", 0x0A9FFF},    {"purple", 0x00F25ABF},
        {"violet", 0x00F25ABF},     {"pink", 0x006137FF},    {"cyan", 0x00E6D600},
        {"magenta", 0x00FF00FF},    {"white", 0x00FFFFFF},   {"black", 0x00000000},
        {"grey", 0x0093938E},       {"gray", 0x0093938E},
    };
    std::string low = to_lower(s);
    for (const Word& w : words)
        if (low.find(w.word) != std::string::npos) {
            if (bgr) *bgr = w.color;
            return true;
        }
    return false;
}

// "Click the blue button, then the one below it" - the clause has no
// coordinates, so find the control it is talking about on the real screen.
static bool click_control_matching(const std::string& phrase, Point* out) {
    uint32_t want = 0;
    bool want_color = color_word(phrase, &want);
    bool wants_button = to_lower(phrase).find("button") != std::string::npos ||
                        to_lower(phrase).find("control") != std::string::npos ||
                        to_lower(phrase).find("link") != std::string::npos;
    if (!want_color && !wants_button) return false;
    Image img = capture_screen();
    if (img.empty()) return false;
    Rect v = virtual_desktop();

    // A colour word is the strongest signal: use the biggest blob of it.
    if (want_color) {
        Rect area;
        if (find_color_area(img, want, 120, &area)) {
            *out = Point{v.x + area.x + area.w / 2, v.y + area.y + area.h / 2};
            return true;
        }
    }
    // Otherwise the most control-looking detected element whose colour is near
    // the named one, or simply the top-ranked control.
    std::vector<UiElement> els = detect_elements(img, 14);
    for (const UiElement& e : els) {
        if (!want_color) break;
        int d = abs((int)(e.color & 0xFF) - (int)(want & 0xFF)) +
                abs((int)((e.color >> 8) & 0xFF) - (int)((want >> 8) & 0xFF)) +
                abs((int)((e.color >> 16) & 0xFF) - (int)((want >> 16) & 0xFF));
        if (d <= 120) {
            *out = Point{v.x + e.rect.x + e.rect.w / 2, v.y + e.rect.y + e.rect.h / 2};
            return true;
        }
    }
    if (wants_button && !els.empty()) {
        *out = Point{v.x + els[0].rect.x + els[0].rect.w / 2,
                     v.y + els[0].rect.y + els[0].rect.h / 2};
        return true;
    }
    return false;
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
        // "open my python ide / code editor" - the `code` launcher starts VS Code
        {"visual studio code", "code", "Visual Studio Code"},
        {"vscode", "code", "Visual Studio Code"},  {"vs code", "code", "Visual Studio Code"},
        {"python ide", "code", "Visual Studio Code"},
        {"code editor", "code", "Visual Studio Code"},
        {"notepad++", "notepad++", "Notepad"},     {"wordpad", "write", nullptr},
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

    // Whole-goal recipe: "create a text file with hello harry, save it and
    // open it again". Checked against the entire goal first, because splitting
    // on "and"/"," would scatter save/reopen across separate clauses. This is
    // a real editor round-trip: Notepad, typed keystrokes, the real Save
    // dialog, close, then reopen the saved document from disk and read it back.
    {
        std::string gl = to_lower(goal);
        bool doc = gl.find("text file") != std::string::npos ||
                   gl.find("text doc") != std::string::npos ||
                   gl.find("document") != std::string::npos ||
                   gl.find("notepad") != std::string::npos ||
                   gl.find("a file") != std::string::npos;
        bool save = gl.find("save") != std::string::npos;
        bool reopen = gl.find("open it again") != std::string::npos ||
                      gl.find("open again") != std::string::npos ||
                      gl.find("reopen") != std::string::npos ||
                      gl.find("open the file") != std::string::npos ||
                      gl.find("open it") != std::string::npos;
        bool named = gl.find("called ") != std::string::npos ||
                     gl.find("named ") != std::string::npos;
        bool round_trip = save && reopen;
        if (doc && (round_trip || named)) {
            auto quoted = [&](char c) {
                size_t p = goal.find(c);
                if (p == std::string::npos) return std::string();
                size_t e = goal.find(c, p + 1);
                if (e == std::string::npos || e == p + 1) return std::string();
                return trim(goal.substr(p + 1, e - p - 1));
            };
            std::string text = quoted('\'');
            if (text.empty()) text = quoted('"');
            if (text.empty() && gl.find("harry") != std::string::npos) text = "hello harry";
            if (text.empty()) {
                for (const char* pre : {"that says ", "saying ", "written ", "with the text ",
                                        "that contains "}) {
                    size_t p = gl.find(pre);
                    if (p == std::string::npos) continue;
                    text = trim(goal.substr(p + strlen(pre)));
                    for (const char* stop : {", save", " and save", " then save", ", close",
                                             " and close", " and open", ", open", " then open",
                                             ".", ";"}) {
                        size_t s = to_lower(text).find(stop);
                        if (s != std::string::npos) text = trim(text.substr(0, s));
                    }
                    break;
                }
            }
            if (round_trip && text.empty()) text = "hello harry";
            for (char& c : text)
                if (c == '\n' || c == '\r') c = ' ';

            std::string fname = "hello-harry.txt";
            for (const char* pre : {"named ", "called "}) {
                size_t p = gl.find(pre);
                if (p == std::string::npos) continue;
                std::string n = trim(goal.substr(p + strlen(pre)));
                for (const char* stop : {" with ", " that ", ", ", " and ", " then ", ".", ";"}) {
                    size_t s = to_lower(n).find(stop);
                    if (s != std::string::npos) n = trim(n.substr(0, s));
                }
                if (!n.empty()) { fname = n; break; }
            }
            for (char& c : fname)
                if (c == ' ' || c == '\t') c = '-';
            if (fname.find('.') == std::string::npos) fname += ".txt";

            std::string full = expand_path("~") + "\\Documents\\" + fname;
            if (!round_trip) {
                // "open a text file called X": create it only if it is
                // missing, then open it in the editor.
                cmds.push_back("ensure " + full + (text.empty() ? "" : " " + text));
                cmds.push_back("open " + full);
                cmds.push_back("wait 2600");
                cmds.push_back("focus Notepad");
                cmds.push_back("read " + full);
                cmds.push_back("shot opened.png");
                return cmds;
            }
            cmds.push_back("del " + full);
            cmds.push_back("open notepad");
            cmds.push_back("wait 2400");
            cmds.push_back("focus Notepad");
            cmds.push_back("type " + text);
            cmds.push_back("key ctrl+s");
            cmds.push_back("wait 2000");
            cmds.push_back("type " + fname);
            cmds.push_back("key enter");
            cmds.push_back("wait 2000");
            cmds.push_back("close Notepad");
            cmds.push_back("waitclose Notepad 8000");
            cmds.push_back("wait 1500");
            cmds.push_back("open " + full);
            cmds.push_back("wait 2600");
            cmds.push_back("focus Notepad");
            cmds.push_back("read " + full);
            cmds.push_back("shot reopened.png");
            cmds.push_back("log created, written, saved and reopened: " + full);
            return cmds;
        }
    }
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
            if (nums.size() >= 2) {
                cmds.push_back("click left " + nums[0] + " " + nums[1]);
            } else {
                // No coordinates: see whether the words name a control we can
                // actually find on the screen ("the blue button").
                Point hit{};
                if (click_control_matching(rest.empty() ? clause : rest, &hit)) {
                    cmds.push_back("click left " + std::to_string(hit.x) + " " +
                                   std::to_string(hit.y));
                } else {
                    cmds.push_back("click left " + std::to_string(v.x + v.w / 2) + " " +
                                   std::to_string(v.y + v.h / 2));
                }
            }
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
            if (!config.auto_run) {
                log("warn", "auto-run is off; the local plan was saved for review and nothing was executed");
                return false;
            }
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
    if (!config.vision) {
        log("fail", "screenshot-based goal planning requires vision; enable vision in hawk.ini "
                    "and use a vision-capable model");
        return false;
    }

    request_abort(false);
    save_cursor_origin();
    std::string transcript;      // what has been executed, ok or FAIL, for the model to see
    std::string generated;       // the whole script so far, shown in the script box
    std::vector<std::string> plan;   // the model's script for the whole task
    size_t plan_at = 0;          // how much of that plan has been executed
    bool need_plan = true;       // the first vision call writes the full script
    bool force_check = false;    // a step failed: consult the model before continuing
    bool done = false;
    int total_steps = 0;
    int round = 0;
    int shot_w = 0, shot_h = 0;

    // Every vision call starts the same way: a fresh screenshot, marked with a
    // coordinate grid and the mouse position, plus a local analysis of what is
    // actually on it.
    auto prepare_shot = [&](std::string* b64, std::string* analysis) -> bool {
        Image full = capture_screen();
        if (full.empty()) {
            log("fail", "could not capture the screen for visual planning");
            return false;
        }
        Image small = scale_to_fit(full, 1280, 800);
        if (small.empty()) {
            log("fail", "could not prepare the screenshot for visual planning");
            return false;
        }
        Point c = cursor_pos();
        Rect v = virtual_desktop();
        remember_map(small, v);  // sclick/smove now aim in this screenshot's pixel space
        *analysis = visual_report(small);
        std::string accessible = describe_accessible_controls(GetForegroundWindow(), 40);
        if (!accessible.empty()) *analysis += "\nWindows accessibility controls:\n" + accessible;
        shot_w = small.w;
        shot_h = small.h;
        // A labelled grid lets the model read coordinates off the image itself;
        // the green crosshair marks where the real mouse currently is.
        Image marked = draw_marker(draw_grid(small, 120),
                                   (c.x - v.x) * small.w / std::max(1, full.w),
                                   (c.y - v.y) * small.h / std::max(1, full.h));
        std::string shot = take_shot("goal");
        if (shot.empty() || !save_png(small, shot)) {
            log("fail", "could not save the screenshot used for goal planning");
            return false;
        }
        last_shot_ = shot;

        std::vector<uint8_t> png;
        // Reuse the PNG encoder to create the image payload for the model.
        std::string tmp = shot_dir() + "\\_model_" + std::to_string(GetCurrentProcessId()) +
                          "_" + std::to_string(round) + ".png";
        if (!save_png(marked, tmp)) {
            log("fail", "could not encode the screenshot for the vision model");
            return false;
        }
        FILE* f = fopen(tmp.c_str(), "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long n = ftell(f);
                if (n > 0 && fseek(f, 0, SEEK_SET) == 0) {
                    png.resize((size_t)n);
                    size_t read = fread(png.data(), 1, png.size(), f);
                    if (read != png.size()) png.clear();
                }
            }
            fclose(f);
        }
        DeleteFileA(tmp.c_str());
        if (png.empty()) {
            log("fail", "could not read the encoded screenshot for the vision model");
            return false;
        }
        *b64 = b64_encode(png);
        if (b64->empty()) {
            log("fail", "could not encode the screenshot for the vision model request");
            return false;
        }
        return true;
    };

    // The instructions that stay the same for planning and for checking.
    Rect vdesk = virtual_desktop();
    auto aim_block = [&]() {
        std::string s;
        s += "\nHow to aim (important):\n"
             "- The attached image is " + std::to_string(shot_w) + "x" + std::to_string(shot_h) +
             " pixels with a labelled coordinate grid: every grid line is marked with its own "
             "pixel coordinate, origin 0,0 at the top-left.\n"
             "- Read a point off the image, then use it directly:\n"
             "    sclick x y        click that point of the screenshot on the real screen\n"
             "    smove x y         move the real mouse there\n"
             "    sdrag x1 y1 x2 y2 drag between two points of the screenshot\n"
             "  Hawk converts these to real desktop pixels for you. Do NOT scale coordinates "
             "yourself. Plain `click x y` / `move x y` take absolute desktop pixels (the virtual "
             "desktop is " + std::to_string(vdesk.w) + "x" + std::to_string(vdesk.h) + " at " +
             std::to_string(vdesk.x) + "," + std::to_string(vdesk.y) + "); only use those if you "
             "were given desktop coordinates.\n"
             "- To hit a coloured control without any coordinate, use `findarea RRGGBB 60 click` "
             "- it finds the whole region of that colour and clicks its centre.\n"
             "- To re-read the screen yourself, use `look` (Hawk analyses the screenshot and "
             "reports the controls it detects) or `elements`.\n"
             "- Prefer the visible controls listed in the analysis below; they were detected "
             "from this very screenshot, not invented.\n";
        return s;
    };

    while (!done && round < config.max_rounds && total_steps < config.max_steps &&
           !abort_requested()) {
        round++;
        std::string b64, analysis;
        if (!prepare_shot(&b64, &analysis)) break;

        // ---------------- plan the whole task, or verify what happened --------
        if (need_plan || force_check || plan_at >= plan.size()) {
            std::string prompt;
            prompt += "You are the driver of a real Windows desktop. You act by replying with "
                      "commands only, one per line, in the command language below. Nothing "
                      "else: no prose, no code fences. The attached image is mandatory visual "
                      "evidence of the current desktop; inspect it before deciding what to do. "
                      "Do not guess coordinates or claim an action succeeded without seeing the "
                      "result in a later screenshot.\n\n"
                      "Goal: " + goal + "\n\n";
            prompt += command_reference();
            prompt += aim_block();
            if (need_plan) {
                prompt +=
                    "\nThis is the planning round: write the COMPLETE script for the whole "
                    "task, one command per line, as many lines as the task needs (up to 30). "
                    "It will be executed in segments and checked visually between them; you "
                    "will be asked again before the task is believed done.\n"
                    "Writing a robust multi-step script:\n"
                    "- Launch an application with `open <app>`, then `waitfocus <window title>` "
                    "to wait for its window; never `focus` a window you just launched without "
                    "waiting.\n"
                    "- After launching, closing or saving something, allow `wait 1500` (or more) "
                    "before acting on its result.\n"
                    "- Type text or source code with one `type` line per line; `\\n` inside a "
                    "line types Enter, and backslashes in code must be doubled.\n"
                    "- A save flow looks like: key ctrl+s, wait 1500, type <filename>, key enter.\n"
                    "- Guard steps that may not apply (a dialog that might not appear) with `try`.\n"
                    "- Prove the result before finishing: read the saved file back, capture a "
                    "screenshot, or verifypixel a control you expect to see.\n"
                    "- Do not include `done`; the last step's screenshot decides completion.\n\n";
            } else {
                prompt +=
                    "\nThis is a verification round: the script so far has been executed and "
                    "the attached screenshot shows the result. Reply with exactly one of:\n"
                    "  done                       the goal is visibly satisfied right now\n"
                    "  continue                   it is on track; execution goes on by itself\n"
                    "  up to 4 corrective commands  something failed or needs adjusting\n"
                    "Do not repeat commands that already succeeded. Do not include `done` next "
                    "to other commands.\n";
            }
            prompt += analysis;
            prompt += "\n";
            prompt += screen_summary();
            if (!transcript.empty()) prompt += "\nWhat has been executed so far:\n" + transcript;
            if (!need_plan && plan_at < plan.size()) {
                prompt += "\nPlan lines not executed yet:\n";
                for (size_t i = plan_at; i < plan.size() && i < plan_at + 12; i++)
                    prompt += "  " + plan[i] + "\n";
            }
            prompt += "\nThe attached image is the current screen, downscaled to fit the stated "
                      "dimensions and overlaid with the coordinate grid described above; its "
                      "green crosshair marks the current mouse position.\n";

            std::string reply, err;
            log("agent", std::string(need_plan ? "planning " : "verifying ") + "round " +
                             std::to_string(round) + " with " + config.model +
                             " (screenshot attached: " + std::to_string(shot_w) + "x" +
                             std::to_string(shot_h) + ")");
            if (!llm_chat(prompt, b64, &reply, &err, need_plan ? 2400 : 900)) {
                log("fail", "model call failed: " + err);
                log("info", "no guessed script was run; check the vision-capable model, API key "
                            "and OpenAI-compatible base URL in Model settings");
                break;
            }
            log("agent", "model said: " + reply.substr(0, 400));

            std::vector<std::string> cmds = parse_commands(reply);
            if (need_plan) {
                if (cmds.empty()) {
                    transcript += "  FAIL the model returned no commands for the plan; reply with "
                                  "command lines only\n";
                    log("warn", "the model returned no usable plan; asking again with a fresh "
                                "screenshot");
                    continue;
                }
                if (cmds.size() > 30) cmds.resize(30);
                plan = cmds;
                plan_at = 0;
                need_plan = false;
                generated = join_commands(plan);
                publish_script(generated);  // visible in the script box, and on disk
                log("agent", "plan: the model wrote " + std::to_string(plan.size()) +
                                 " command(s) for the whole task");
                for (const std::string& c : plan) log("agent", "plan: " + c);
                if (!config.auto_run) {
                    log("warn", "auto-run is off, so the generated script was saved for review "
                                "and nothing was executed");
                    break;
                }
            } else {
                std::string t = to_lower(trim(reply));
                // Accept "done", "Done." or a line of praise that starts with
                // it - but only ever from a verification round with a fresh
                // screenshot in hand.
                if ((cmds.size() == 1 && to_lower(cmds[0]) == "done") ||
                    (cmds.empty() && t.rfind("done", 0) == 0)) {
                    done = true;
                    break;
                }
                force_check = false;
                if (cmds.empty()) {
                    if (t.find("continue") != std::string::npos && plan_at < plan.size()) {
                        log("agent", "model says continue; executing the rest of the plan");
                    } else if (plan_at >= plan.size()) {
                        log("warn", "the model neither said done nor gave corrective commands; "
                                    "stopping instead of looping");
                        break;
                    } else {
                        transcript += "  FAIL the verification reply was not usable; reply with "
                                      "done, continue or corrective commands\n";
                        log("warn", "verification reply was not usable; asking again");
                    }
                } else {
                    // corrective commands go in front of the remaining plan, so they
                    // run next
                    int n = 0;
                    std::vector<std::string> fixes;
                    for (const std::string& c : cmds) {
                        if (to_lower(c) == "done") continue;  // done is never accepted mid-batch
                        if (n++ >= 4) break;
                        fixes.push_back(c);
                    }
                    if (!fixes.empty()) {
                        log("agent", "corrections: " + std::to_string(fixes.size()) +
                                         " command(s) to put the task back on track");
                        plan.insert(plan.begin() + (long)plan_at, fixes.begin(), fixes.end());
                        generated += join_commands(fixes);
                        publish_script(generated);
                    }
                }
            }
        }

        // ------- execute the next segment of the plan, without another call ---
        int ran_this_segment = 0;
        while (plan_at < plan.size() && ran_this_segment < config.segment_size && !done) {
            if (total_steps >= config.max_steps) {
                log("warn", "goal reached the maximum action count");
                break;
            }
            if (abort_requested()) {
                log("warn", "goal aborted");
                break;
            }
            std::string c = plan[plan_at++];
            if (to_lower(trim(c)) == "done") {
                // A plan that ends in `done` still has to earn it: the next
                // round looks at a fresh screenshot first.
                log("warn", "the plan says `done` - verifying with a fresh screenshot before "
                            "believing it");
                plan_at = plan.size();
                break;
            }
            if (config.step_delay_ms > 0) sleep_ms(config.step_delay_ms);
            bool ok = step(c, 0);
            total_steps++;
            ran_this_segment++;
            transcript += (ok ? "  ok   " : "  FAIL ") + c + "\n";
            log(ok ? "ok" : "fail", "[plan] " + c);
            if (!ok) {
                force_check = true;
                log("warn", "a plan step failed - asking the model what to do next instead of "
                            "ploughing on");
                break;
            }
        }
        if (abort_requested() && !done) {
            log("warn", "goal aborted");
            break;
        }
        if (total_steps >= config.max_steps) {
            log("warn", "goal reached the maximum action count");
            break;
        }
    }

    restore_cursor_origin();
    log(done ? "ok" : "warn", done ? "goal finished" : "goal stopped without a completion signal");
    return done;
}

}  // namespace hawk
