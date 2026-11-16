#include "ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "isa.h"
#include "misc/cpp/imgui_stdlib.h"
#include "samples.h"

namespace {

float g_scale = 1.0f;
inline float px(float v) { return v * g_scale; }

const ImU32 COL_STAGE[5] = {
    IM_COL32(79, 163, 255, 255),   // IF  blue
    IM_COL32(61, 214, 181, 255),   // ID  teal
    IM_COL32(255, 180, 84, 255),   // EX  amber
    IM_COL32(197, 139, 255, 255),  // MEM purple
    IM_COL32(255, 111, 145, 255)   // WB  pink
};
const char* const STAGE_NAME[5] = {"IF", "ID", "EX", "MEM", "WB"};
const char* const STAGE_LONG[5] = {"Fetch", "Decode", "Execute", "Memory", "Write back"};
const char* const STAGE_UNIT[5] = {"Instruction memory", "Registers + control", "ALU", "Data cache", "Register write"};

const ImU32 COL_TEXT = IM_COL32(226, 232, 240, 255);
const ImU32 COL_DIM = IM_COL32(124, 136, 154, 255);
const ImU32 COL_FAINT = IM_COL32(58, 68, 84, 255);
const ImU32 COL_BOX = IM_COL32(27, 33, 44, 255);
const ImU32 COL_RED = IM_COL32(255, 96, 96, 255);
const ImU32 COL_GREEN = IM_COL32(98, 216, 130, 255);
const ImU32 COL_YELLOW = IM_COL32(255, 221, 87, 255);
const ImU32 COL_CYAN = IM_COL32(110, 220, 255, 255);
const ImU32 COL_ORANGE = IM_COL32(255, 150, 70, 255);

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 fade(ImU32 c, float a) {
    ImVec4 v = v4(c);
    v.w *= a;
    return ImGui::ColorConvertFloat4ToU32(v);
}
float flash(double at, double now, float dur = 1.2f) {
    float f = 1.0f - static_cast<float>((now - at) / dur);
    return f < 0 ? 0.0f : (f > 1 ? 1.0f : f);
}

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof buf, f, args);
    va_end(args);
    return buf;
}

// Text clipped to (and optionally centered in) the rectangle a..b.
void text_in(ImDrawList* dl, ImFont* font, ImVec2 a, ImVec2 b, ImU32 col, const std::string& s, bool center = true) {
    const float size = font->FontSize;
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str());
    float x = center ? a.x + std::max(0.0f, (b.x - a.x - ts.x) * 0.5f) : a.x;
    float y = a.y + (b.y - a.y - ts.y) * 0.5f;
    dl->PushClipRect(a, b, true);
    dl->AddText(font, size, ImVec2(x, y), col, s.c_str());
    dl->PopClipRect();
}

void arrow_head(ImDrawList* dl, ImVec2 tip, ImVec2 dir, ImU32 col, float size) {
    ImVec2 n(-dir.y, dir.x);
    ImVec2 base(tip.x - dir.x * size, tip.y - dir.y * size);
    dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * size * 0.6f, base.y + n.y * size * 0.6f),
                          ImVec2(base.x - n.x * size * 0.6f, base.y - n.y * size * 0.6f), col);
}

const ImGuiWindowFlags PANEL = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

void begin_panel(const char* title, ImVec2 pos, ImVec2 size, ImGuiWindowFlags extra = 0) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(title, nullptr, PANEL | extra);
}

// "add $t0, $t1, $t2" -> "add $t0,$t1,$t2" so it fits in a stage token.
std::string compact(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++)
        if (!(s[i] == ' ' && i > 0 && s[i - 1] == ',')) out += s[i];
    return out;
}

// One-sentence summary of a cycle in plain words.
std::string describe(const Trace& t) {
    std::vector<std::string> parts;
    if (t.mem_stall) {
        parts.push_back(fmt("cache miss at 0x%03X: pipeline frozen, %d more cycle%s", t.mem.addr, t.mem_stall_left,
                            t.mem_stall_left == 1 ? "" : "s"));
    } else {
        if (t.fwd_a != Fwd::None)
            parts.push_back(fmt("ALU input A forwarded from %s", t.fwd_a == Fwd::ExMem ? "EX/MEM" : "MEM/WB"));
        if (t.fwd_b != Fwd::None)
            parts.push_back(fmt("input B forwarded from %s", t.fwd_b == Fwd::ExMem ? "EX/MEM" : "MEM/WB"));
        if (t.stall_id)
            parts.push_back(fmt("ID stalls waiting for %s, a bubble enters EX", isa::reg_name(t.stall_reg)));
        if (t.branch_resolved)
            parts.push_back(t.branch_taken ? fmt("branch taken to 0x%04X, IF and ID flushed", t.redirect)
                                           : std::string("branch not taken"));
        if (t.jump) parts.push_back(fmt("jump to 0x%04X, fetched instruction flushed", t.redirect));
        if (t.mem.happened)
            parts.push_back(fmt("%s 0x%03X: cache %s%s", t.mem.write ? "store to" : "load from", t.mem.addr,
                                t.mem.hit ? "hit" : "miss", t.mem.wrote_back ? " (dirty block written back)" : ""));
        if (t.wb_write) parts.push_back(fmt("%s = %d", isa::reg_name(t.wb_reg), t.wb_value));
    }
    if (parts.empty()) return "nothing special this cycle";
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? ";  " : "") + parts[i];
    return out;
}

// ---------------------------------------------------------------- toolbar
void draw_toolbar(App& a, ImVec2 pos, ImVec2 size, double now) {
    begin_panel("##toolbar", pos, size, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    const Stats& st = a.sim.state().stats;

    ImGui::PushFont(a.font_title);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(v4(COL_CYAN), "MIPS Pipeline Simulator");
    ImGui::PopFont();
    ImGui::SameLine(0, px(24));

    const bool halted = a.sim.halted();
    ImGui::BeginDisabled(halted);
    ImGui::PushStyleColor(ImGuiCol_Button, a.running ? ImVec4(0.62f, 0.38f, 0.12f, 1) : ImVec4(0.13f, 0.50f, 0.30f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a.running ? ImVec4(0.75f, 0.47f, 0.16f, 1) : ImVec4(0.17f, 0.62f, 0.38f, 1));
    if (ImGui::Button(a.running ? "Pause" : "Run", ImVec2(px(84), 0))) a.running = !a.running;
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Step", ImVec2(px(64), 0))) {
        a.running = false;
        a.advance(now);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!a.sim.can_step_back());
    if (ImGui::Button("Back", ImVec2(px(64), 0))) {
        a.running = false;
        a.step_back();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(px(64), 0))) a.reset();

    ImGui::SameLine(0, px(20));
    ImGui::SetNextItemWidth(px(120));
    ImGui::SliderFloat("Speed", &a.speed, 0.5f, 60.0f, "%.1f Hz", ImGuiSliderFlags_Logarithmic);

    ImGui::SameLine(0, px(20));
    int mode = static_cast<int>(a.sim.cfg.hazard);
    ImGui::SetNextItemWidth(px(170));
    if (ImGui::Combo("Hazards", &mode, "None (wrong results)\0Stall only\0Forwarding\0"))
        a.sim.cfg.hazard = static_cast<HazardMode>(mode);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How data hazards are handled.\nNone: dependent instructions read stale registers.\n"
                          "Stall only: wait in ID until the value is written back.\n"
                          "Forwarding: bypass results to the ALU; stall once on load-use.");

    ImGui::SameLine(0, px(20));
    ImGui::SetNextItemWidth(px(110));
    ImGui::SliderInt("Miss penalty", &a.sim.cfg.miss_penalty, 0, 10, "%d cycles");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extra cycles the pipeline freezes on every data-cache miss.");

    ImGui::SameLine(0, px(20));
    if (ImGui::Checkbox("Regs = 0x100+i", &a.sim.cfg.course_regs)) a.reset();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Initial register values from the course assignment (otherwise 0).");

    // Second row: statistics.
    ImGui::PushFont(a.font_mono);
    auto stat = [&](const char* label, const std::string& value, ImU32 col = COL_TEXT) {
        ImGui::TextColored(v4(COL_DIM), "%s", label);
        ImGui::SameLine(0, px(6));
        ImGui::TextColored(v4(col), "%s", value.c_str());
        ImGui::SameLine(0, px(22));
    };
    stat("Cycle", fmt("%llu", static_cast<unsigned long long>(st.cycles)), COL_CYAN);
    stat("Instructions", fmt("%llu", static_cast<unsigned long long>(st.retired)));
    stat("CPI", st.retired ? fmt("%.2f", static_cast<double>(st.cycles) / st.retired) : std::string("-"));
    stat("Stalls", fmt("%llu", static_cast<unsigned long long>(st.data_stalls)), COL_ORANGE);
    stat("Flushed", fmt("%llu", static_cast<unsigned long long>(st.flushed)), COL_RED);
    stat("Forwards", fmt("%llu", static_cast<unsigned long long>(st.forwards)), COL_YELLOW);
    stat("Branches", fmt("%llu/%llu taken", static_cast<unsigned long long>(st.taken),
                         static_cast<unsigned long long>(st.branches)));
    const uint64_t accesses = st.hits + st.misses;
    stat("Cache", accesses ? fmt("%llu hit / %llu miss (%.0f%%)", static_cast<unsigned long long>(st.hits),
                                 static_cast<unsigned long long>(st.misses), 100.0 * st.hits / accesses)
                           : std::string("no accesses"),
         COL_GREEN);
    if (a.challenge) stat("Score", fmt("%d (streak %d)", a.score, a.streak), COL_YELLOW);
    if (st.mem_stall_cycles) stat("Miss wait", fmt("%llu", static_cast<unsigned long long>(st.mem_stall_cycles)));
    if (halted) ImGui::TextColored(v4(COL_GREEN), a.sim.program().empty() ? "no program loaded" : "HALTED - program finished");
    else ImGui::NewLine();
    ImGui::PopFont();
    ImGui::End();
}

// ---------------------------------------------------------------- editor
void draw_editor(App& a, ImVec2 pos, ImVec2 size) {
    begin_panel("Assembly editor", pos, size);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(140));
    if (ImGui::BeginCombo("##sample", a.lesson >= 0 ? "(lesson program)" : SAMPLES[a.sample].name)) {
        for (int i = 0; i < SAMPLE_COUNT; i++)
            if (ImGui::Selectable(SAMPLES[i].name, i == a.sample)) a.load_sample(i);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.38f, 0.66f, 1));
    if (ImGui::Button("Assemble & load", ImVec2(-1, 0))) a.assemble_and_load();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Enter");

    const float footer = ImGui::GetTextLineHeightWithSpacing() + px(2);
    ImGui::PushFont(a.font_mono);
    ImGui::InputTextMultiline("##src", &a.source, ImVec2(-1, ImGui::GetContentRegionAvail().y - footer),
                              ImGuiInputTextFlags_AllowTabInput);
    if (ImGui::IsItemFocused() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) a.assemble_and_load();
    ImGui::PopFont();
    if (!a.error.empty()) ImGui::TextColored(v4(COL_RED), "%s", a.error.c_str());
    else ImGui::TextColored(v4(COL_DIM), "%d instructions loaded", static_cast<int>(a.sim.program().size()));
    ImGui::End();
}

// ---------------------------------------------------------------- program listing
void draw_listing(App& a, ImVec2 pos, ImVec2 size) {
    begin_panel("Program (disassembly)", pos, size);
    const auto& prog = a.sim.program();
    const Trace* t = a.sim.history().empty() ? nullptr : &a.sim.history().back();

    // Which stage (if any) each instruction is in. Earliest stage wins for the row color.
    std::vector<int> stage_of(prog.size(), -1);
    if (t)
        for (int s = 4; s >= 0; s--)
            if (t->stage[s].valid && t->stage[s].pc / 4 < prog.size()) stage_of[t->stage[s].pc / 4] = s;

    ImGui::PushFont(a.font_mono);
    if (ImGui::BeginTable("listing", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Stg", 0, px(30));
        ImGui::TableSetupColumn("Addr", 0, px(44));
        ImGui::TableSetupColumn("Machine", 0, px(72));
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < prog.size(); i++) {
            const uint32_t addr = static_cast<uint32_t>(i * 4);
            auto label = a.loaded.labels.find(addr);
            if (label != a.loaded.labels.end()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(v4(COL_YELLOW), "%s:", label->second.c_str());
            }
            ImGui::TableNextRow();
            const int s = stage_of[i];
            if (s >= 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, fade(COL_STAGE[s], 0.22f));
            ImGui::TableSetColumnIndex(0);
            if (s >= 0) {
                ImGui::TextColored(v4(COL_STAGE[s]), "%s", STAGE_NAME[s]);
                if (s == 0 && a.scroll_listing) ImGui::SetScrollHereY(0.5f);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(v4(COL_DIM), "%04X", addr);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(v4(COL_DIM), "%08X", prog[i]);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(compact(isa::disassemble(prog[i], addr)).c_str());
        }
        ImGui::EndTable();
    }
    a.scroll_listing = false;
    ImGui::PopFont();
    ImGui::End();
}

// ---------------------------------------------------------------- datapath diagram
void draw_datapath(App& a, ImVec2 pos, ImVec2 size, double now) {
    begin_panel("Pipeline datapath", pos, size, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const auto& hist = a.sim.history();
    const Trace* t = hist.empty() ? nullptr : &hist.back();
    const Trace* prev = hist.size() >= 2 ? &hist[hist.size() - 2] : nullptr;

    const float bar_w = px(14), gap = px(7);
    const float box_w = (avail.x - 4 * (bar_w + 2 * gap)) / 5;
    const float arc_h = px(44), note_h = px(22);
    const float top = p0.y + px(2);
    const float bottom = p0.y + avail.y - arc_h - note_h;
    float box_x[5], bar_x[4];
    for (int i = 0; i < 5; i++) box_x[i] = p0.x + i * (box_w + bar_w + 2 * gap);
    for (int i = 0; i < 4; i++) bar_x[i] = box_x[i] + box_w + gap;

    const float token_y = top + px(62), token_h = px(30);
    const float mid_y = token_y + token_h * 0.5f;
    const bool frozen = t && t->mem_stall;

    // Click targets: every stage box and pipeline register can be inspected.
    static const char* const BAR_NAME[4] = {"IF/ID", "ID/EX", "EX/MEM", "MEM/WB"};
    bool hot_box[5], hot_bar[4];
    for (int i = 0; i < 5; i++) {
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(box_x[i], top));
        if (ImGui::InvisibleButton("stage", ImVec2(box_w, bottom - top))) a.select(Selection::Stage, i);
        hot_box[i] = ImGui::IsItemHovered();
        if (hot_box[i]) ImGui::SetTooltip("%s stage: click to see what it is doing", STAGE_LONG[i]);
        if (i < 4) {
            ImGui::SetCursorScreenPos(ImVec2(box_x[i] + box_w, top));
            if (ImGui::InvisibleButton("bar", ImVec2(bar_w + 2 * gap, bottom - top))) a.select(Selection::Latch, i);
            hot_bar[i] = ImGui::IsItemHovered();
            if (hot_bar[i]) ImGui::SetTooltip("%s pipeline register: click to learn about it", BAR_NAME[i]);
        }
        ImGui::PopID();
        if (hot_box[i] || (i < 4 && hot_bar[i])) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::SetCursorScreenPos(p0);

    // Stage boxes.
    for (int i = 0; i < 5; i++) {
        ImVec2 a0(box_x[i], top), a1(box_x[i] + box_w, bottom);
        dl->AddRectFilled(a0, a1, COL_BOX, px(8));
        dl->AddRectFilled(a0, ImVec2(a1.x, top + px(26)), fade(COL_STAGE[i], 0.22f), px(8), ImDrawFlags_RoundCornersTop);
        const bool lit = hot_box[i] || a.sel.is(Selection::Stage, i);
        dl->AddRect(a0, a1, lit ? COL_STAGE[i] : fade(COL_STAGE[i], frozen ? 0.35f : 0.8f), px(8), 0, lit ? px(3) : px(1.5f));
        dl->AddText(a.font_title, a.font_title->FontSize, ImVec2(a0.x + px(9), top + px(3)), COL_STAGE[i], STAGE_NAME[i]);
        ImVec2 name_size = a.font_title->CalcTextSizeA(a.font_title->FontSize, FLT_MAX, 0, STAGE_NAME[i]);
        text_in(dl, a.font_ui, ImVec2(a0.x + px(15) + name_size.x, top), ImVec2(a1.x - px(4), top + px(26)), COL_DIM,
                STAGE_LONG[i], false);
        text_in(dl, a.font_ui, ImVec2(a0.x + px(4), top + px(32)), ImVec2(a1.x - px(4), top + px(54)), COL_DIM, STAGE_UNIT[i]);
    }
    // Pipeline registers between the stages.
    for (int i = 0; i < 4; i++) {
        ImVec2 b0(bar_x[i], top + px(8)), b1(bar_x[i] + bar_w, bottom - px(8));
        const bool lit = hot_bar[i] || a.sel.is(Selection::Latch, i);
        dl->AddRectFilled(b0, b1, lit ? IM_COL32(96, 112, 140, 255) : IM_COL32(52, 62, 80, 255), px(3));
        dl->AddRect(b0, b1, lit ? COL_TEXT : IM_COL32(92, 106, 130, 255), px(3));
        // Wires in and out, with a clock-edge pulse right after a step.
        float pulse = flash(a.step_time, now, 0.35f);
        ImU32 wire = fade(COL_TEXT, 0.18f + 0.5f * pulse);
        dl->AddLine(ImVec2(box_x[i] + box_w, mid_y), ImVec2(b0.x, mid_y), wire, px(2));
        dl->AddLine(ImVec2(b1.x, mid_y), ImVec2(box_x[i + 1], mid_y), wire, px(2));
        arrow_head(dl, ImVec2(box_x[i + 1], mid_y), ImVec2(1, 0), wire, px(6));
    }

    if (!t) {
        text_in(dl, a.font_ui, ImVec2(p0.x, bottom), ImVec2(p0.x + avail.x, p0.y + avail.y), COL_DIM,
                a.sim.program().empty() ? "Assemble a program to begin."
                                        : "Press Step (right arrow) or Run (space) to clock the pipeline. Click any box to learn about it.");
        ImGui::Dummy(avail);
        ImGui::End();
        return;
    }

    // Slide animation timing.
    float dur = a.running ? std::clamp(0.8f / a.speed, 0.05f, 0.28f) : 0.28f;
    float k = std::clamp(static_cast<float>((now - a.step_time) / dur), 0.0f, 1.0f);
    k = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
    if (!a.slide) k = 1.0f;

    // Detail lines under each token.
    std::string line1[5], line2[5];
    ImU32 col2[5] = {COL_DIM, COL_DIM, COL_DIM, COL_DIM, COL_DIM};
    const State& s = a.sim.state();
    if (t->stage[0].valid) line1[0] = fmt("PC = 0x%04X", t->stage[0].pc);
    else line1[0] = "PC past end";
    if (t->stage[1].valid) {
        if (t->stall_id) {
            line1[1] = fmt("waits for %s", isa::reg_name(t->stall_reg));
        } else if (!t->squashed[1] && !frozen && s.id_ex.info.valid && s.id_ex.info.seq == t->stage[1].seq) {
            const ID_EX& d = s.id_ex;
            if (d.uses_rs && d.uses_rt) line1[1] = fmt("A=%d  B=%d", d.a, d.b);
            else if (d.uses_rs) line1[1] = fmt("A=%d  imm=%d", d.a, d.imm);
            else if (d.uses_rt) line1[1] = fmt("B=%d", d.b);
            if (d.reg_write) line2[1] = fmt("dest %s", isa::reg_name(d.dest));
        }
    }
    if (t->ex_valid) {
        line1[2] = fmt("%d %s %d = %d", t->alu_a, alu_symbol(t->alu), t->alu_b, t->alu_result);
        if (t->branch_resolved) {
            line2[2] = t->branch_taken ? fmt("TAKEN -> 0x%04X", t->redirect) : std::string("not taken");
            col2[2] = t->branch_taken ? COL_RED : COL_DIM;
        } else if (t->fwd_a != Fwd::None || t->fwd_b != Fwd::None) {
            line2[2] = std::string("forwarded ") + (t->fwd_a != Fwd::None ? "A" : "") +
                       (t->fwd_a != Fwd::None && t->fwd_b != Fwd::None ? "+" : "") + (t->fwd_b != Fwd::None ? "B" : "");
            col2[2] = COL_YELLOW;
        }
    }
    if (t->mem_stall) {
        line1[3] = fmt("[0x%03X] wait %d", t->mem.addr, t->mem_stall_left + 1);
    } else if (t->mem.happened) {
        line1[3] = fmt("%s [0x%03X] %s %d", t->mem.write ? "W" : "R", t->mem.addr, t->mem.write ? "<-" : "=", t->mem.value);
        line2[3] = t->mem.hit ? "cache HIT" : (t->mem.wrote_back ? "MISS+write-back" : "cache MISS");
        col2[3] = t->mem.hit ? COL_GREEN : COL_RED;
    } else if (t->stage[3].valid) {
        line1[3] = "no memory access";
    }
    if (t->wb_write) {
        line1[4] = fmt("%s <- %d", isa::reg_name(t->wb_reg), t->wb_value);
        line2[4] = fmt("0x%08X", static_cast<uint32_t>(t->wb_value));
    } else if (t->stage[4].valid && !frozen) {
        line1[4] = "no reg write";
    }
    for (int i = 0; i < 5; i++) {
        ImVec2 l0(box_x[i] + px(4), token_y + token_h + px(8));
        ImVec2 l1(box_x[i] + box_w - px(4), l0.y + px(18));
        text_in(dl, a.font_mono, l0, l1, COL_TEXT, line1[i]);
        text_in(dl, a.font_mono, ImVec2(l0.x, l1.y), ImVec2(l1.x, l1.y + px(18)), col2[i], line2[i]);
    }

    // Badges.
    auto badge = [&](int stage, const char* text, ImU32 col) {
        ImVec2 ts = a.font_ui->CalcTextSizeA(a.font_ui->FontSize, FLT_MAX, 0, text);
        ImVec2 b1(box_x[stage] + box_w - px(6), bottom - px(6));
        ImVec2 b0(b1.x - ts.x - px(12), b1.y - ts.y - px(4));
        dl->AddRectFilled(b0, b1, fade(col, 0.9f), px(4));
        dl->AddText(a.font_ui, a.font_ui->FontSize, ImVec2(b0.x + px(6), b0.y + px(2)), IM_COL32(18, 20, 26, 255), text);
    };
    if (t->stall_id) {
        badge(0, "HELD", COL_ORANGE);
        badge(1, "STALL", COL_ORANGE);
    }
    if (frozen)
        for (int i = 0; i < 5; i++) badge(i, i == 3 ? "CACHE MISS" : "FROZEN", i == 3 ? COL_RED : COL_FAINT);
    if (t->squashed[0]) badge(0, "FLUSH", COL_RED);
    if (t->squashed[1]) badge(1, "FLUSH", COL_RED);

    // Instruction tokens (drawn last so they slide over the register bars).
    for (int i = 0; i < 5; i++) {
        const StageInfo& in = t->stage[i];
        const float w = box_w - px(12);
        if (!in.valid) {
            text_in(dl, a.font_mono, ImVec2(box_x[i] + px(6), token_y), ImVec2(box_x[i] + px(6) + w, token_y + token_h),
                    COL_FAINT, i == 0 ? "(end of program)" : "bubble");
            continue;
        }
        float x = box_x[i] + px(6);
        float alpha = 1.0f;
        ImU32 col = COL_STAGE[i];
        if (prev && i > 0 && prev->stage[i - 1].valid && prev->stage[i - 1].seq == in.seq) {
            x = box_x[i - 1] + px(6) + (x - box_x[i - 1] - px(6)) * k;
        } else if (i == 0 && (!prev || !prev->stage[0].valid || prev->stage[0].seq != in.seq)) {
            alpha = k;
        }
        ImVec2 t0(x, token_y), t1(x + w, token_y + token_h);
        const bool dead = t->squashed[i];
        dl->AddRectFilled(ImVec2(t0.x + px(2), t0.y + px(3)), ImVec2(t1.x + px(2), t1.y + px(3)), IM_COL32(0, 0, 0, 90), px(6));
        dl->AddRectFilled(t0, t1, fade(dead ? IM_COL32(90, 50, 56, 255) : col, alpha), px(6));
        text_in(dl, a.font_mono, ImVec2(t0.x + px(5), t0.y), ImVec2(t1.x - px(5), t1.y),
                fade(dead ? IM_COL32(255, 170, 170, 255) : IM_COL32(14, 18, 24, 255), alpha),
                compact(isa::disassemble(in.instr, in.pc)));
        if (dead) {
            dl->AddLine(ImVec2(t0.x + px(6), mid_y), ImVec2(t1.x - px(6), mid_y), COL_RED, px(2));
            dl->AddRect(t0, t1, COL_RED, px(6), 0, px(1.5f));
        }
    }

    // Arcs under the boxes: forwarding paths and PC redirects.
    const float glow = 0.55f + 0.45f * std::max(flash(a.step_time, now, 0.6f), 0.0f);
    auto arc = [&](float from_x, float to_x, float depth, ImU32 col, const std::string& label) {
        ImVec2 s0(from_x, bottom + px(1)), s1(to_x, bottom + px(7));
        ImVec2 c0(from_x, bottom + depth * 1.7f), c1(to_x, bottom + depth * 1.7f);
        dl->AddBezierCubic(s0, c0, c1, s1, fade(col, glow), px(2.5f));
        arrow_head(dl, ImVec2(to_x, bottom + px(1)), ImVec2(0, -1), fade(col, glow), px(8));
        ImVec2 ts = a.font_ui->CalcTextSizeA(a.font_ui->FontSize, FLT_MAX, 0, label.c_str());
        float lx = (from_x + to_x) * 0.5f - ts.x * 0.5f, ly = bottom + depth * 1.28f - ts.y * 0.5f;
        dl->AddRectFilled(ImVec2(lx - px(4), ly - px(1)), ImVec2(lx + ts.x + px(4), ly + ts.y + px(1)),
                          ImGui::GetColorU32(ImGuiCol_WindowBg), px(3));
        dl->AddText(a.font_ui, a.font_ui->FontSize, ImVec2(lx, ly), col, label.c_str());
    };
    auto fwd_src = [&](Fwd f) { return bar_x[f == Fwd::ExMem ? 2 : 3] + bar_w * 0.5f; };
    if (t->fwd_a != Fwd::None)
        arc(fwd_src(t->fwd_a), box_x[2] + box_w * 0.30f, t->fwd_a == Fwd::ExMem ? px(14) : px(24), COL_YELLOW,
            t->fwd_a == Fwd::ExMem ? "A" : "fwd A");
    if (t->fwd_b != Fwd::None)
        arc(fwd_src(t->fwd_b), box_x[2] + box_w * 0.70f, t->fwd_b == Fwd::ExMem ? px(14) : px(24), COL_CYAN,
            t->fwd_b == Fwd::ExMem ? "B" : "fwd B");
    if (t->branch_resolved && t->branch_taken)
        arc(box_x[2] + box_w * 0.12f, box_x[0] + box_w * 0.5f, px(24), COL_RED, fmt("branch taken: PC = 0x%04X", t->redirect));
    if (t->jump) arc(box_x[1] + box_w * 0.5f, box_x[0] + box_w * 0.5f, px(20), COL_RED, fmt("jump: PC = 0x%04X", t->redirect));

    // What happened, in words.
    ImVec2 n0(p0.x, p0.y + avail.y - note_h), n1(p0.x + avail.x, p0.y + avail.y);
    text_in(dl, a.font_ui, n0, n1, COL_TEXT,
            fmt("Cycle %llu:  ", static_cast<unsigned long long>(t->cycle)) + describe(*t), false);

    ImGui::Dummy(avail);
    ImGui::End();
}

// ---------------------------------------------------------------- learn panel
enum { TAB_INSPECT, TAB_LESSONS, TAB_CHALLENGE, TAB_LATCHES };

void heading(App& a, const std::string& text, ImU32 col) {
    ImGui::PushFont(a.font_title);
    ImGui::TextColored(v4(col), "%s", text.c_str());
    ImGui::PopFont();
}

void bullet(const std::string& text) {
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text.c_str());
}

bool green_button(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.50f, 0.30f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.62f, 0.38f, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    return pressed;
}

void draw_inspect(App& a) {
    ImGui::BeginChild("inspect");
    if (a.sel.kind == Selection::None) {
        heading(a, "Click anything to learn about it", COL_CYAN);
        ImGui::TextWrapped("This is a working model of a 5-stage MIPS processor. Everything you see is live, and most of "
                           "it can be clicked:");
        bullet("A stage box in the datapath above (IF, ID, EX, MEM, WB) explains what that stage is for and what it is "
               "doing to its instruction right now.");
        bullet("A grey bar between two stages explains pipeline registers.");
        bullet("A register or a cache row on the right explains itself and what is about to happen to it.");
        bullet("The Lessons tab is a guided tour. The Challenge tab quizzes you on what the next clock cycle will do.");
    } else {
        const Explanation e = explain(a.sim, a.sel);
        heading(a, e.title, e.color_stage >= 0 ? COL_STAGE[e.color_stage] : COL_CYAN);
        ImGui::TextColored(v4(COL_DIM), "WHAT IT IS FOR");
        ImGui::TextWrapped("%s", e.what.c_str());
        ImGui::Spacing();
        ImGui::TextColored(v4(COL_DIM), "RIGHT NOW");
        for (const std::string& line : e.now) bullet(line);
    }
    ImGui::EndChild();
}

void draw_lessons(App& a) {
    const auto& all = lessons();
    ImGui::BeginChild("lessons");
    if (a.lesson < 0) {
        ImGui::TextWrapped("Short guided tours. Each one loads a tiny program and walks you through what the pipeline "
                           "does with it, one idea at a time.");
        ImGui::Spacing();
        for (int i = 0; i < static_cast<int>(all.size()); i++) {
            if (ImGui::Button(all[i].title, ImVec2(px(230), 0))) a.start_lesson(i);
            ImGui::SameLine();
            ImGui::TextColored(v4(COL_DIM), "%s", all[i].blurb);
        }
    } else {
        const Lesson& lesson = all[a.lesson];
        const int count = static_cast<int>(lesson.steps.size());
        const bool last = a.lesson_step + 1 >= count;
        heading(a, lesson.title, COL_CYAN);
        ImGui::SameLine();
        ImGui::TextColored(v4(COL_DIM), "  step %d of %d", a.lesson_step + 1, count);
        ImGui::TextWrapped("%s", lesson.steps[a.lesson_step].text);
        ImGui::Spacing();
        if (a.lesson_running) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(v4(COL_YELLOW), "Clocking the pipeline...");
        } else if (!last) {
            if (green_button("Continue  (Enter)", ImVec2(px(160), 0))) a.lesson_next();
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(v4(COL_GREEN), "Lesson complete.");
            if (a.lesson + 1 < static_cast<int>(all.size())) {
                ImGui::SameLine();
                if (green_button("Next lesson", ImVec2(px(120), 0))) a.start_lesson(a.lesson + 1);
            }
        }
        ImGui::SameLine(0, px(20));
        if (ImGui::Button("Restart lesson")) a.start_lesson(a.lesson);
        ImGui::SameLine();
        if (ImGui::Button("All lessons")) {
            a.lesson = -1;
            a.lesson_running = false;
        }
    }
    ImGui::EndChild();
}

void draw_challenge(App& a, double now) {
    ImGui::BeginChild("challenge");
    if (!a.challenge) {
        heading(a, "Predict the pipeline", COL_CYAN);
        ImGui::TextWrapped("Turn this on, then Step or Run any program. Just before something interesting happens, the "
                           "clock stops and you are asked to predict it: is the branch taken, will the cache hit, "
                           "where does a value get forwarded from, does the instruction have to stall? Right answers "
                           "score points, and a streak of right answers scores more.");
        ImGui::Spacing();
        if (green_button("Start challenge", ImVec2(px(160), 0))) {
            a.challenge = true;
            a.score = a.streak = a.best_streak = a.asked = a.right = 0;
        }
        if (a.asked) {
            ImGui::SameLine(0, px(16));
            ImGui::TextColored(v4(COL_DIM), "Last game: %d points, %d of %d correct, best streak %d", a.score, a.right,
                               a.asked, a.best_streak);
        }
    } else {
        ImGui::PushFont(a.font_mono);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(v4(COL_YELLOW), "Score %d", a.score);
        ImGui::SameLine(0, px(22));
        ImGui::TextColored(v4(a.streak >= 3 ? COL_ORANGE : COL_TEXT), "Streak %d", a.streak);
        ImGui::SameLine(0, px(22));
        ImGui::TextColored(v4(COL_DIM), "Best %d    Correct %d/%d", a.best_streak, a.right, a.asked);
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - px(110));
        if (ImGui::Button("Stop challenge", ImVec2(px(110), 0))) {
            a.challenge = false;
            a.quiz = Quiz{};
        }
        ImGui::Separator();
        const Quiz& q = a.quiz;
        if (!q.active) {
            ImGui::TextWrapped(a.sim.halted() ? "The program has finished. Press Reset, or load another program, to keep playing."
                                              : "Press Step (right arrow) or Run (space). The clock will stop when there "
                                                "is something to predict.");
        } else {
            ImGui::TextWrapped("%s", q.question.c_str());
            ImGui::Spacing();
            for (int i = 0; i < static_cast<int>(q.options.size()); i++) {
                int pushed = 0;
                if (q.chosen >= 0 && i == q.correct) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.50f, 0.30f, 1));
                    pushed = 1;
                } else if (q.chosen == i) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.20f, 0.22f, 1));
                    pushed = 1;
                }
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
                if (ImGui::Button(fmt("  %d.   %s", i + 1, q.options[i].c_str()).c_str(), ImVec2(-1, 0))) a.answer(i);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(pushed);
            }
            if (q.chosen >= 0) {
                ImGui::Spacing();
                if (q.chosen == q.correct) ImGui::TextColored(v4(COL_GREEN), "Correct!  +%d points", a.last_points);
                else ImGui::TextColored(v4(COL_RED), "Not quite.");
                ImGui::TextWrapped("%s", q.why.c_str());
                if (green_button("Continue  (Enter)", ImVec2(px(160), 0))) a.quiz_continue(now);
            } else {
                ImGui::TextColored(v4(COL_DIM), "Click an answer or press its number.");
            }
        }
    }
    ImGui::EndChild();
}

void draw_latch_table(App& a);

void draw_learn(App& a, ImVec2 pos, ImVec2 size, double now) {
    begin_panel("Learn", pos, size);
    if (ImGui::BeginTabBar("learntabs")) {
        const int want = a.tab_request;
        a.tab_request = -1;
        auto tab = [&](int id, const char* label) {
            return ImGui::BeginTabItem(label, nullptr, want == id ? ImGuiTabItemFlags_SetSelected : 0);
        };
        if (tab(TAB_INSPECT, "Inspect")) {
            draw_inspect(a);
            ImGui::EndTabItem();
        }
        if (tab(TAB_LESSONS, "Lessons")) {
            draw_lessons(a);
            ImGui::EndTabItem();
        }
        if (tab(TAB_CHALLENGE, a.challenge ? "Challenge (on)###challenge" : "Challenge###challenge")) {
            draw_challenge(a, now);
            ImGui::EndTabItem();
        }
        if (tab(TAB_LATCHES, "Pipeline registers")) {
            draw_latch_table(a);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- pipeline register contents
void draw_latch_table(App& a) {
    const State& s = a.sim.state();
    ImGui::PushFont(a.font_mono);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(2)));

    auto row = [&](const char* name, const std::string& value, bool on = true) {
        ImGui::TextColored(v4(COL_DIM), "%-10s", name);
        ImGui::SameLine(0, px(4));
        ImGui::TextColored(v4(on ? COL_TEXT : COL_FAINT), "%s", value.c_str());
    };
    auto flag = [&](const char* name, bool v) { row(name, v ? "1" : "0", v); };
    auto head = [&](const char* name, int stage, const StageInfo& in) {
        ImGui::TextColored(v4(COL_STAGE[stage]), "%s", name);
        if (in.valid) ImGui::TextUnformatted(isa::disassemble(in.instr, in.pc).c_str());
        else ImGui::TextColored(v4(COL_FAINT), "(bubble)");
        ImGui::Separator();
    };

    if (ImGui::BeginTable("latches", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        head("IF/ID", 1, s.if_id.info);
        if (s.if_id.info.valid) {
            row("Instr", fmt("0x%08X", s.if_id.info.instr));
            row("PC+4", fmt("0x%04X", s.if_id.info.pc + 4));
        }
        ImGui::Spacing();
        ImGui::TextColored(v4(COL_STAGE[0]), "PC");
        row("next", fmt("0x%04X", s.pc));

        ImGui::TableSetColumnIndex(1);
        head("ID/EX", 2, s.id_ex.info);
        if (s.id_ex.info.valid) {
            const ID_EX& d = s.id_ex;
            row("A (rs)", fmt("%d", d.a), d.uses_rs);
            row("B (rt)", fmt("%d", d.b), d.uses_rt);
            row("Imm", fmt("%d", d.imm));
            row("Dest", isa::reg_name(d.dest), d.reg_write);
            row("ALUOp", alu_name(d.alu));
            flag("ALUSrc", d.alu_src);
            flag("RegWrite", d.reg_write);
            flag("MemRead", d.mem_read);
            flag("MemWrite", d.mem_write);
            flag("MemToReg", d.mem_to_reg);
            flag("Branch", d.branch != 0);
        }

        ImGui::TableSetColumnIndex(2);
        head("EX/MEM", 3, s.ex_mem.info);
        if (s.ex_mem.info.valid) {
            const EX_MEM& d = s.ex_mem;
            row("ALUResult", fmt("%d", d.alu_result));
            row("", fmt("0x%08X", static_cast<uint32_t>(d.alu_result)), false);
            row("StoreVal", fmt("%d", d.store_value), d.mem_write);
            row("Dest", isa::reg_name(d.dest), d.reg_write);
            flag("RegWrite", d.reg_write);
            flag("MemRead", d.mem_read);
            flag("MemWrite", d.mem_write);
            flag("MemToReg", d.mem_to_reg);
        }

        ImGui::TableSetColumnIndex(3);
        head("MEM/WB", 4, s.mem_wb.info);
        if (s.mem_wb.info.valid) {
            const MEM_WB& d = s.mem_wb;
            row("MemData", fmt("%d", d.mem_data), d.mem_to_reg);
            row("ALUResult", fmt("%d", d.alu_result), !d.mem_to_reg);
            row("Dest", isa::reg_name(d.dest), d.reg_write);
            flag("RegWrite", d.reg_write);
            flag("MemToReg", d.mem_to_reg);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// ---------------------------------------------------------------- pipeline diagram (instruction x cycle)
void draw_gantt(App& a, ImVec2 pos, ImVec2 size) {
    begin_panel("Pipeline diagram (instructions x cycles)", pos, size,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const auto& hist = a.sim.history();

    const float label_w = px(178), cell_w = px(34), cell_h = px(20), head_h = px(20);
    const int max_cols = std::max(1, static_cast<int>((avail.x - label_w) / cell_w));
    const int max_rows = std::max(1, static_cast<int>((avail.y - head_h) / cell_h));
    const size_t first = hist.size() > static_cast<size_t>(max_cols) ? hist.size() - max_cols : 0;

    struct Row {
        uint64_t seq;
        StageInfo info;
    };
    std::vector<Row> rows;
    std::unordered_map<uint64_t, int> row_of;
    for (size_t c = first; c < hist.size(); c++)
        for (int s = 4; s >= 0; s--) {
            const StageInfo& in = hist[c].stage[s];
            if (in.valid && !row_of.count(in.seq)) {
                row_of[in.seq] = static_cast<int>(rows.size());
                rows.push_back({in.seq, in});
            }
        }
    const int skip = std::max(0, static_cast<int>(rows.size()) - max_rows);

    if (hist.empty()) {
        text_in(dl, a.font_ui, p0, ImVec2(p0.x + avail.x, p0.y + avail.y), COL_DIM,
                "Each row is one instruction; each column one clock cycle.");
    }
    // Column headers.
    for (size_t c = first; c < hist.size(); c++) {
        float x = p0.x + label_w + (c - first) * cell_w;
        const bool last = c + 1 == hist.size();
        if (last)
            dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + cell_w, p0.y + avail.y), IM_COL32(255, 255, 255, 12));
        text_in(dl, a.font_mono, ImVec2(x, p0.y), ImVec2(x + cell_w, p0.y + head_h), last ? COL_TEXT : COL_DIM,
                fmt("%llu", static_cast<unsigned long long>(hist[c].cycle)));
    }
    // Row labels.
    for (int r = skip; r < static_cast<int>(rows.size()); r++) {
        float y = p0.y + head_h + (r - skip) * cell_h;
        text_in(dl, a.font_mono, ImVec2(p0.x + px(2), y), ImVec2(p0.x + label_w - px(6), y + cell_h), COL_TEXT,
                isa::disassemble(rows[r].info.instr, rows[r].info.pc), false);
    }
    // Cells.
    for (size_t c = first; c < hist.size(); c++) {
        const Trace& t = hist[c];
        float x = p0.x + label_w + (c - first) * cell_w;
        for (int s = 0; s < 5; s++) {
            if (!t.stage[s].valid) continue;
            int r = row_of[t.stage[s].seq];
            if (r < skip) continue;
            float y = p0.y + head_h + (r - skip) * cell_h;
            ImVec2 c0(x + px(1), y + px(1)), c1(x + cell_w - px(1), y + cell_h - px(1));
            const bool held = t.mem_stall || (t.stall_id && s <= 1);
            const bool dead = t.squashed[s];
            dl->AddRectFilled(c0, c1, dead ? IM_COL32(110, 44, 52, 255) : fade(COL_STAGE[s], held ? 0.45f : 1.0f), px(3));
            text_in(dl, a.font_mono, c0, c1, dead ? IM_COL32(255, 170, 170, 255) : IM_COL32(14, 18, 24, 255), STAGE_NAME[s]);
            if (dead) dl->AddLine(ImVec2(c0.x + px(3), (c0.y + c1.y) * 0.5f), ImVec2(c1.x - px(3), (c0.y + c1.y) * 0.5f), COL_RED, px(1.5f));
        }
    }
    ImGui::Dummy(avail);
    ImGui::End();
}

// ---------------------------------------------------------------- registers
void draw_registers(App& a, ImVec2 pos, ImVec2 size, double now) {
    begin_panel("Registers", pos, size);
    const State& s = a.sim.state();
    ImGui::PushFont(a.font_mono);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(6), px(1.5f)));
    if (ImGui::BeginTable("regs", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        for (int r = 0; r < 16; r++) {
            ImGui::TableNextRow();
            for (int c = 0; c < 2; c++) {
                ImGui::TableSetColumnIndex(c);
                const int i = r + 16 * c;
                const float f = flash(a.reg_at[i], now, 1.5f);
                if (f > 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, fade(COL_STAGE[4], 0.55f * f));
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                if (ImGui::Selectable("##reg", a.sel.is(Selection::Reg, i), ImGuiSelectableFlags_AllowOverlap))
                    a.select(Selection::Reg, i);
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::SetCursorScreenPos(cell);
                ImGui::TextColored(v4(COL_DIM), "%2d", i);
                ImGui::SameLine(0, px(6));
                ImGui::TextColored(v4(COL_CYAN), "%-5s", isa::reg_name(i));
                ImGui::SameLine(0, px(6));
                ImGui::TextColored(v4(s.regs[i] ? COL_TEXT : COL_FAINT), "%08X", static_cast<uint32_t>(s.regs[i]));
                ImGui::SameLine(0, px(8));
                ImGui::TextColored(v4(s.regs[i] ? COL_DIM : COL_FAINT), "%d", s.regs[i]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::End();
}

// ---------------------------------------------------------------- cache + memory
void draw_cache(App& a, ImVec2 pos, ImVec2 size, double now) {
    begin_panel("Data cache and memory", pos, size);
    const State& s = a.sim.state();
    const Trace* t = a.sim.history().empty() ? nullptr : &a.sim.history().back();
    const bool acc = t && t->mem.happened;

    if (ImGui::BeginTabBar("memtabs")) {
        if (ImGui::BeginTabItem("Cache (direct-mapped, 16 x 16 bytes)")) {
            ImGui::PushFont(a.font_mono);
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(5), px(1.5f)));
            if (ImGui::BeginTable("cache", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Slot");
                ImGui::TableSetupColumn("V");
                ImGui::TableSetupColumn("D");
                ImGui::TableSetupColumn("Tag");
                ImGui::TableSetupColumn("Data  +0 +1 +2 ...", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (int i = 0; i < CACHE_SLOTS; i++) {
                    const CacheLine& l = s.cache.line[i];
                    ImGui::TableNextRow();
                    const float f = flash(a.slot_at[i], now, 1.5f);
                    const bool here = acc && t->mem.slot == i;
                    if (f > 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, fade(here && !t->mem.hit ? COL_RED : COL_GREEN, 0.40f * f));
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 cell = ImGui::GetCursorScreenPos();
                    ImGui::PushID(i);
                    if (ImGui::Selectable("##slot", a.sel.is(Selection::Slot, i),
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                        a.select(Selection::Slot, i);
                    ImGui::PopID();
                    ImGui::SameLine();
                    ImGui::SetCursorScreenPos(cell);
                    ImGui::TextColored(v4(COL_DIM), "%X", i);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(v4(l.valid ? COL_GREEN : COL_FAINT), l.valid ? "1" : "0");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(v4(l.dirty ? COL_ORANGE : COL_FAINT), l.dirty ? "1" : "0");
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextColored(v4(l.valid ? COL_TEXT : COL_FAINT), "%X", l.tag);
                    ImGui::TableSetColumnIndex(4);
                    for (int j = 0; j < BLOCK_SIZE; j++) {
                        if (j) ImGui::SameLine(0, px(5));
                        const uint32_t addr = s.cache.block_base(i) + j;
                        const bool touched = here && l.valid && addr >= t->mem.addr && addr < t->mem.addr + t->mem.size;
                        ImGui::TextColored(v4(!l.valid ? COL_FAINT : touched ? COL_YELLOW : COL_TEXT), "%02X", l.data[j]);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            ImGui::PopFont();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Main memory (2048 bytes)")) {
            ImGui::TextColored(v4(COL_DIM), "Cyan = block is in the cache. Orange = cache holds a newer value (not written back yet).");
            ImGui::PushFont(a.font_mono);
            ImGui::BeginChild("mem");
            ImGuiListClipper clip;
            clip.Begin(MEM_SIZE / 16);
            while (clip.Step())
                for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                    ImGui::TextColored(v4(COL_DIM), "%03X:", row * 16);
                    for (int j = 0; j < 16; j++) {
                        const uint32_t addr = static_cast<uint32_t>(row * 16 + j);
                        ImGui::SameLine(0, px(j == 8 ? 12 : 6));
                        const bool cached = s.cache.probe(addr);
                        const bool stale = cached && a.sim.peek8(addr) != s.mem[addr];
                        ImGui::TextColored(v4(stale ? COL_ORANGE : cached ? COL_CYAN : COL_TEXT), "%02X", s.mem[addr]);
                    }
                }
            ImGui::EndChild();
            ImGui::PopFont();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace

// ================================================================ App

App::App() {
    for (double& v : reg_at) v = -100.0;
    for (double& v : slot_at) v = -100.0;
    load_sample(0);
}

void App::apply_style() {
    g_scale = scale;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.WindowBorderSize = 1;
    st.WindowPadding = ImVec2(8, 8);
    st.ItemSpacing = ImVec2(8, 5);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.090f, 0.120f, 1);
    c[ImGuiCol_Border] = ImVec4(0.17f, 0.21f, 0.28f, 1);
    c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.17f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.17f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.15f, 0.20f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.20f, 0.27f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.23f, 0.32f, 1);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.20f, 0.28f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.28f, 0.39f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.34f, 0.48f, 1);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.24f, 0.34f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.30f, 0.43f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.11f, 0.14f, 0.19f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.15f, 0.21f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.28f, 0.42f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.33f, 0.48f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.11f, 0.15f, 1);
    c[ImGuiCol_Text] = v4(COL_TEXT);
    st.ScaleAllSizes(scale);
}

void App::load_sample(int index) {
    sample = index;
    lesson = -1;
    source = SAMPLES[index].source;
    sim.cfg.course_regs = SAMPLES[index].course_regs;
    assemble_and_load();
}

bool App::assemble_and_load() {
    AsmResult r = assemble(source);
    if (!r.ok) {
        error = "Line " + std::to_string(r.error_line) + ": " + r.error;
        return false;
    }
    error.clear();
    loaded = r;
    sim.load(r.words);
    reset();
    return true;
}

void App::reset() {
    sim.reset();
    running = false;
    accum = 0;
    step_time = -100.0;
    slide = false;
    for (double& v : reg_at) v = -100.0;
    for (double& v : slot_at) v = -100.0;
    scroll_listing = true;
    quiz = Quiz{};
    lesson_running = false;
}

void App::select(Selection::Kind kind, int index) {
    sel.kind = kind;
    sel.index = index;
    tab_request = TAB_INSPECT;
}

void App::advance(double now) {
    if (quiz.active) return;  // answer first
    if (challenge && make_quiz(sim, quiz)) {
        quiz.active = true;
        resume_run = running;
        running = false;
        tab_request = TAB_CHALLENGE;
        return;
    }
    step(now);
}

void App::answer(int option) {
    if (!quiz.active || quiz.chosen >= 0 || option < 0 || option >= static_cast<int>(quiz.options.size())) return;
    quiz.chosen = option;
    asked++;
    if (option == quiz.correct) {
        right++;
        streak++;
        best_streak = std::max(best_streak, streak);
        last_points = 10 + 5 * std::min(streak - 1, 8);
        score += last_points;
    } else {
        streak = 0;
        last_points = 0;
    }
}

void App::quiz_continue(double now) {
    if (!quiz.active || quiz.chosen < 0) return;
    quiz = Quiz{};
    step(now);
    running = resume_run && !sim.halted();
}

void App::start_lesson(int index) {
    const Lesson& l = lessons()[index];
    source = l.source;
    sim.cfg.hazard = static_cast<HazardMode>(l.hazard);
    sim.cfg.miss_penalty = l.penalty;
    sim.cfg.course_regs = false;
    challenge = false;
    assemble_and_load();
    lesson = index;
    lesson_step = 0;
    tab_request = TAB_LESSONS;
}

void App::lesson_next() {
    if (lesson < 0 || lesson_running) return;
    const Lesson& l = lessons()[lesson];
    if (lesson_step + 1 >= static_cast<int>(l.steps.size())) return;
    lesson_step++;
    const LessonStep& st = l.steps[lesson_step];
    if (st.hazard >= 0) sim.cfg.hazard = static_cast<HazardMode>(st.hazard);
    if (st.restart) reset();
    running = false;
    lesson_running = !sim.halted();
}

void App::lesson_tick(double now) {
    if (!lesson_running || lesson < 0) return;
    step(now);
    const LessonStep& st = lessons()[lesson].steps[lesson_step];
    if (sim.halted() || sim.history().empty() || until_met(st.until, sim.history().back())) lesson_running = false;
}

void App::step(double now) {
    if (!sim.step()) {
        running = false;
        return;
    }
    const Trace& t = sim.history().back();
    step_time = now;
    slide = true;
    scroll_listing = true;
    if (t.wb_write) reg_at[t.wb_reg] = now;
    if (t.mem.happened) slot_at[t.mem.slot] = now;
    if (sim.halted()) running = false;
}

void App::step_back() {
    if (sim.step_back()) {
        quiz = Quiz{};
        lesson_running = false;
        slide = false;
        scroll_listing = true;
    }
}

void App::frame(double now, float dt) {
    ImGuiIO& io = ImGui::GetIO();

    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (io.WantTextInput) {
        // The editor has the keyboard.
    } else if (quiz.active) {
        for (int i = 0; i < static_cast<int>(quiz.options.size()); i++)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false) ||
                ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad1 + i), false))
                answer(i);
        if (quiz.chosen >= 0 && (enter || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))) quiz_continue(now);
    } else {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !sim.halted()) running = !running;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_F10)) {
            running = false;
            advance(now);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            running = false;
            step_back();
        }
        if (enter && lesson >= 0) lesson_next();
    }
    if (lesson_running) {
        accum += static_cast<double>(dt) * std::max(speed, 3.0f);
        while (accum >= 1.0 && lesson_running) {
            lesson_tick(now);
            accum -= 1.0;
        }
    } else if (running) {
        accum += static_cast<double>(dt) * speed;
        int budget = 8;
        while (accum >= 1.0 && running && budget-- > 0) {
            advance(now);
            accum -= 1.0;
        }
        if (accum > 1.0) accum = 1.0;
    } else {
        accum = 0;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 o = vp->WorkPos, sz = vp->WorkSize;
    const float m = px(6);
    const float bar_h = px(74);
    const float y0 = o.y + m + bar_h + m;
    const float h = sz.y - bar_h - 3 * m;
    const float left_w = std::floor(sz.x * 0.215f), right_w = std::floor(sz.x * 0.275f);
    const float mid_w = sz.x - left_w - right_w - 4 * m;
    const float xl = o.x + m, xm = xl + left_w + m, xr = xm + mid_w + m;

    ImGui::PushFont(font_ui);
    draw_toolbar(*this, ImVec2(o.x + m, o.y + m), ImVec2(sz.x - 2 * m, bar_h), now);

    const float editor_h = std::floor(h * 0.52f);
    draw_editor(*this, ImVec2(xl, y0), ImVec2(left_w, editor_h));
    draw_listing(*this, ImVec2(xl, y0 + editor_h + m), ImVec2(left_w, h - editor_h - m));

    const float path_h = px(262), latch_h = px(322);
    draw_datapath(*this, ImVec2(xm, y0), ImVec2(mid_w, path_h), now);
    draw_learn(*this, ImVec2(xm, y0 + path_h + m), ImVec2(mid_w, latch_h), now);
    draw_gantt(*this, ImVec2(xm, y0 + path_h + latch_h + 2 * m), ImVec2(mid_w, h - path_h - latch_h - 2 * m));

    const float regs_h = px(316);
    draw_registers(*this, ImVec2(xr, y0), ImVec2(right_w, regs_h), now);
    draw_cache(*this, ImVec2(xr, y0 + regs_h + m), ImVec2(right_w, h - regs_h - m), now);
    ImGui::PopFont();
}
