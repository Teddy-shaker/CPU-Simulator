// The whole user interface, drawn with Dear ImGui once per frame.
#pragma once
#include <string>

#include "assembler.h"
#include "cpu.h"
#include "learn.h"

struct ImFont;

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

    float scale = 1.0f;
    ImFont* font_ui = nullptr;
    ImFont* font_mono = nullptr;
    ImFont* font_title = nullptr;

    App();
    void apply_style();
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
