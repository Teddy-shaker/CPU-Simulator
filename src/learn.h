// The teaching layer: explanations for whatever the user clicks, guided
// lessons, and the "predict the next cycle" challenge. No GUI code here.
#pragma once
#include <string>
#include <vector>

#include "cpu.h"

// ---- Click-to-inspect -------------------------------------------------
struct Selection {
    enum Kind { None, Stage, Latch, Reg, Slot } kind = None;
    int index = 0;
    bool is(Kind k, int i) const { return kind == k && index == i; }
};

struct Explanation {
    std::string title;
    int color_stage = -1;          // stage whose color the title uses, -1 for neutral
    std::string what;              // what this part of the CPU is for
    std::vector<std::string> now;  // what it is doing in the current cycle
};

Explanation explain(const Simulator& sim, Selection sel);

// ---- Challenge: predict what the next clock cycle does ------------------
struct Quiz {
    bool active = false;
    std::string question;
    std::vector<std::string> options;
    int correct = 0;
    int chosen = -1;
    std::string why;
};

// Looks one cycle ahead (without changing `sim`) and, if something worth
// predicting is about to happen, fills in a question. Returns false otherwise.
bool make_quiz(Simulator& sim, Quiz& quiz);

// ---- Guided lessons ---------------------------------------------------
enum class Until { Start, Cycle, Forward, Stall, Flush, MemStall, MemAccess, Hit, WriteBack, RegWrite, Halt };

struct LessonStep {
    Until until;        // "Continue" clocks the pipeline until this happens
    int hazard;         // switch hazard mode when the step begins, -1 = keep
    bool restart;       // reset the program when the step begins
    const char* text;
};

struct Lesson {
    const char* title;
    const char* blurb;
    int hazard;
    int penalty;
    const char* source;
    std::vector<LessonStep> steps;
};

const std::vector<Lesson>& lessons();
bool until_met(Until until, const Trace& t);
