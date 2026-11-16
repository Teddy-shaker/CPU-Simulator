// The whole user interface, drawn with Dear ImGui once per frame.
#pragma once
#include <string>

#include "assembler.h"
#include "cpu.h"
#include "learn.h"

struct ImFont;

#define APP_VERSION "1.1.0"

// User preferences, saved between runs.
struct Settings {
    float ui_scale = 1.0f;   // multiplies text and every other size
    int theme = 0;           // 0 midnight blue, 1 black, 2 light
    bool animations = true;
    int reg_format = 0;      // 0 hex + decimal, 1 hex, 2 decimal
    bool show_editor = true, show_listing = true, show_registers = true, show_cache = true, show_diagram = true;
};

struct App {
    Simulator sim;
    std::string source;       // editor text
    AsmResult loaded;         // last program that assembled successfully
    std::string error;        // assembler error for the editor, empty if none
    int sample = 0;

    bool running = false;
    float speed = 3.0f;       // cycles per second while running
    double accum = 0.0;

    // Animation timestamps (seconds).
    double step_time = -100.0;
    bool slide = false;
    double reg_at[32];
    double slot_at[CACHE_SLOTS];
    bool scroll_listing = false;

    // Learning layer.
    Selection sel;            // what the Inspect tab explains
    int tab_request = -1;     // switch the Learn panel to this tab on the next frame
    bool challenge = false;
    Quiz quiz;
    bool resume_run = false;
    int score = 0, last_points = 0, streak = 0, best_streak = 0, asked = 0, right = 0;
    int lesson = -1, lesson_step = 0, lesson_cycles = 0;
    bool lesson_running = false;

    // Preferences and app-level windows.
    Settings settings;
    bool persist = true;          // read/write the settings file
    bool restyle = false;         // main loop must rebuild fonts and style before the next frame
    bool show_settings = false, show_about = false, show_shortcuts = false;
    bool want_quit = false;
    float scale_edit = 100.0f;    // slider value while the Settings window is open
    int high_score = 0;
    float dpi_scale = 1.0f;
    float clear_color[3] = {0.035f, 0.043f, 0.058f};
    std::string backend_info;     // shown in About

    float scale = 1.0f;           // dpi_scale * settings.ui_scale
    ImFont* font_ui = nullptr;
    ImFont* font_mono = nullptr;
    ImFont* font_title = nullptr;

    App();
    void apply_style();
    void set_ui_scale(float s);
    std::string settings_path() const;
    void load_settings();
    void save_settings() const;
    void load_sample(int index);
    bool assemble_and_load();
    void step(double now);       // clock one cycle unconditionally
    void advance(double now);    // clock one cycle, unless the challenge wants an answer first
    void answer(int option);
    void quiz_continue(double now);
    void select(Selection::Kind kind, int index);
    void start_lesson(int index);
    void lesson_next();
    void lesson_tick(double now);
    void step_back();
    void reset();
    void frame(double now, float dt);
};
