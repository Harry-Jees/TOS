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
#include <utility>
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

// ---------------------------------------------------------- vision primitives --
// Everything below is computed locally from real pixels: no model, no network.
// They are what lets a script (or the planner) "see" a screenshot well enough
// to decide where to click.
struct UiElement {
    Rect rect{};          // bounding box, in the coordinates of the image given
    uint32_t color = 0;   // dominant fill colour (BGR, same layout as Image::at)
    int cells = 0;        // how much of the box is that fill colour
    double score = 0;     // higher = more likely to be an interactive control
    const char* kind = "region";  // button | field | panel | region
};

// Rectangular regions of near-uniform colour (buttons, fields, panels,
// toolbars), best first. Used to describe a screenshot to a model and to let
// scripts act on what is actually on screen.
std::vector<UiElement> detect_elements(const Image& img, int max_elements = 12);
// The most common colours, most frequent first, as (BGR, pixel count).
std::vector<std::pair<uint32_t, int>> dominant_colors(const Image& img, int top_n = 6);
// The bounding box of the largest blob of a colour ("that whole blue button",
// not one pixel of it). Empty result -> false.
bool find_color_area(const Image& img, uint32_t bgr, int tolerance, Rect* out);
// Overlay a labelled coordinate grid so a vision model can read positions
// straight off the image instead of guessing at them.
Image draw_grid(const Image& src, int step);
// One-line-per-element description: "1. button 612,455 132x34 #0A84FF centre 678,472".
std::string describe_elements(const std::vector<UiElement>& els);

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
    int max_rounds = 14;           // LLM plan/verify rounds per goal
    int max_steps = 150;           // total commands a goal may execute
    int segment_size = 6;          // planned commands run between vision checks
    bool auto_run = false;         // require explicit opt-in before executing model-generated scripts
    std::string base_url = "https://integrate.api.nvidia.com/v1";
    std::string api_key;
    // The default must actually be able to *see*: Nemotron 3.5 Super VL is
    // NVIDIA's first-party vision-language model on NIM, built for reading
    // screenshots. Any other OpenAI-compatible vision model can be set in
    // hawk.ini, the Model dialog, or HAWK_MODEL.
    std::string model = "nvidia/nemotron-3.5-super-vl-120b-a12b";
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
    // Read the screen the way a person glances at it: capture it, detect the
    // controls on it, and log a plain-text description. Works with no model.
    bool look();
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

    // The screenshot the model last saw, and how its pixel space maps onto the
    // real desktop. `sclick`/`smove`/`sdrag` take coordinates read straight off
    // that image and touch the matching place on the real screen.
    struct ShotMap {
        int w = 0, h = 0;   // screenshot size in its own pixels
        Rect desk{};        // the virtual desktop it came from
        bool valid = false;
    } map_;

    bool step(const std::string& line, int depth);
    void remember_map(const Image& shot, const Rect& desk);
    bool to_desktop(int sx, int sy, Point* out) const;
    // Local, model-free reading of a screenshot: dominant colours + elements.
    std::string visual_report(const Image& img);
    // Show a generated script and keep it in workspace/ so it can be re-run.
    void publish_script(const std::string& script);
    // A small rule-based planner for supported simple goals when no model is
    // configured: turns "open notepad and type hello" into commands to run.
    std::vector<std::string> plan_locally(const std::string& goal);
    std::string screen_summary();
    bool llm_chat(const std::string& prompt, const std::string& image_b64,
                  std::string* reply, std::string* err, int max_tokens = 900);
    std::vector<std::string> parse_commands(const std::string& reply);
};

// ------------------------------------------------------------------- ui -----
int run_gui(HINSTANCE inst, Config cfg);
int run_selftest();                    // proves capture + physical input work
int run_cli(int argc, char** argv);    // CLI mode (see README)
void attach_console();                 // CLI output from a GUI-subsystem exe

}  // namespace hawk
