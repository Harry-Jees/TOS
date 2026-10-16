// Hawk OS - a desktop application that sees the real Windows screen and
// physically drives the real mouse and keyboard.
//
// Core header: shared types, module boundaries, build-time configuration.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#define HAWK_VERSION "1.0.0"
#define HAWK_NAME "Hawk OS"

namespace hawk {

// ---------------------------------------------------------------- strings --
std::wstring wide(const std::string& utf8);
std::string narrow(const std::wstring& utf16);
std::string to_lower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split_words(const std::string& s, int max_parts = 0);
std::wstring format_wide(const wchar_t* fmt, ...);
std::string exe_dir();          // directory containing the running executable
std::string app_dir();          // working directory for shots/, hawk.log, config

// ------------------------------------------------------------------ image --
// 32-bit BGRA, top-down, tightly packed. Coordinates are physical pixels on
// the virtual desktop (which may have negative origins with multiple monitors).
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;

    bool empty() const { return w <= 0 || h <= 0 || px.size() < (size_t)w * h * 4; }
    uint8_t* row(int y) { return px.data() + (size_t)y * w * 4; }
    const uint8_t* row(int y) const { return px.data() + (size_t)y * w * 4; }
    // BGR packed 0x00BBGGRR, the same layout COLORREF and GetPixel use.
    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0xFFFFFFFFu;
        const uint8_t* p = row(y) + (size_t)x * 4;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    }
    void set(int x, int y, uint32_t bgr) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = row(y) + (size_t)x * 4;
        p[0] = (uint8_t)(bgr & 0xFF);
        p[1] = (uint8_t)((bgr >> 8) & 0xFF);
        p[2] = (uint8_t)((bgr >> 16) & 0xFF);
        p[3] = 0xFF;
    }
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

struct Point {
    int x = 0, y = 0;
};

Image capture_screen();                                  // whole virtual desktop
Image capture_region(const Rect& r);                     // clip of the screen
Image capture_window(HWND hwnd);                         // one window's pixels
Image crop(const Image& src, int x, int y, int w, int h);
Image scale_to_fit(const Image& src, int max_w, int max_h);
Image draw_marker(const Image& src, int x, int y);  // crosshair, for the model
std::string color_hex(uint32_t bgr);
uint32_t parse_color(const std::string& hex);  // "RRGGBB" -> BGR, 0xFFFFFFFF on error

bool save_png(const Image& img, const std::string& path);
bool save_bmp(const Image& img, const std::string& path);
std::string b64_encode(const std::vector<uint8_t>& data);
void enable_dpi_awareness();  // physical pixels = input coordinates

// ---------------------------------------------------------------- capture --
// Screen geometry of the whole virtual desktop, in physical pixels.
Rect virtual_desktop();
std::vector<Rect> monitor_rects();
std::string shot_dir();                                   // .../hawk-os/shots
// Capture and write a PNG, returning the path written ("" on failure).
std::string take_shot(const std::string& name_hint);
bool find_color(const Image& img, uint32_t bgr, int tolerance, Point* out);

// ------------------------------------------------------------------ input --
// Every function below acts on the real machine.
// `landed` receives the position the OS reported immediately after the
// injection. That reading is the accurate one: once a human touches the
// physical mouse, Windows resynchronises and the cursor can jump elsewhere.
bool move_cursor(int x, int y, std::string* err = nullptr, Point* landed = nullptr);
bool move_cursor_relative(int dx, int dy, std::string* err = nullptr, Point* landed = nullptr);
Point cursor_pos();
bool click_mouse(const std::string& button, int times, std::string* err = nullptr);
bool drag_mouse(int x1, int y1, int x2, int y2, const std::string& button, std::string* err = nullptr);
bool scroll_wheel(int notches, std::string* err = nullptr);
bool type_text(const std::wstring& text, std::string* err = nullptr);
bool press_combo(const std::string& combo, std::string* err = nullptr);
uint32_t pixel_at(int x, int y);

// Safety: the run loop checks this between every step. It is true while the
// user holds ESC, or after the STOP button / Ctrl+Alt+H hotkey.
bool abort_requested();
void request_abort(bool on);
void save_cursor_origin();
void restore_cursor_origin();

// ---------------------------------------------------------------- windows --
struct WinInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring cls;
    Rect rect{};
    DWORD pid = 0;
    bool visible = false;
    bool minimized = false;
    std::wstring process;
};

std::vector<WinInfo> list_windows(size_t limit = 200);
HWND find_window(const std::string& needle);              // by title substring
std::wstring window_title(HWND hwnd);
bool focus_window(HWND hwnd);
std::string describe_windows(size_t limit);

// ------------------------------------------------------------------ agent --
struct Config {
    bool dry_run = false;          // log actions, do not perform them
    bool restore_cursor = true;    // put the cursor back when a run finishes
    int step_delay_ms = 350;       // pause between actions (visible + safe)
    int max_rounds = 8;            // LLM planning rounds
    int max_steps = 40;            // script steps per round
    bool auto_run = true;          // execute scripts the model generates
    std::string base_url = "https://integrate.api.nvidia.com/v1";
    std::string api_key;
    std::string model = "moonshotai/kimi-k3";  // Kimi K3 on NVIDIA NIM (vision-capable)
    bool vision = true;            // attach screenshots to model requests
};

struct LogLine {
    std::string level;   // info | action | ok | fail | warn | agent
    std::string text;
};

class Agent {
public:
    explicit Agent(Config cfg);
    ~Agent();

    Config config;
    std::function<void(const LogLine&)> on_log;       // UI hook for the log pane
    // Called with each script the planner or the model produces, so the app can
    // show it in the script box while it runs it.
    std::function<void(const std::string&)> on_script;

    // Execute a small command script (see README). Returns true if every
    // non-optional command succeeded.
    bool run_script(const std::string& script);
    // Goal loop: send the goal + a fresh screenshot to the model, execute a
    // small command batch, then look again before planning the next batch.
    // Empty api_key -> use the built-in planner for supported simple goals.
    bool run_goal(const std::string& goal);

    const std::vector<LogLine>& history() const { return history_; }
    std::string last_screenshot() const { return last_shot_; }
    void log(const std::string& level, const std::string& text);

private:
    std::vector<LogLine> history_;
    std::string last_shot_;
    bool stop_ = false;

    bool step(const std::string& line, int depth);
    // Show a generated script and keep it in workspace/ so it can be re-run.
    void publish_script(const std::string& script);
    // A small rule-based planner for supported simple goals when no model is
    // configured: turns "open notepad and type hello" into commands to run.
    std::vector<std::string> plan_locally(const std::string& goal);
    std::string screen_summary();
    bool llm_chat(const std::string& prompt, const std::string& image_b64,
                  std::string* reply, std::string* err);
    std::vector<std::string> parse_commands(const std::string& reply);
};

// ------------------------------------------------------------------- ui -----
int run_gui(HINSTANCE inst, Config cfg);
int run_selftest();                    // proves capture + physical input work
int run_cli(int argc, char** argv);    // CLI mode (see README)
void attach_console();                 // CLI output from a GUI-subsystem exe

}  // namespace hawk
