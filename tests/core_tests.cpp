// Headless tests for the simulator core. Exit code 0 = all passed.
#include <cstdio>
#include <string>

#include "assembler.h"
#include "cpu.h"
#include "isa.h"
#include "learn.h"
#include "samples.h"

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        long long va = static_cast<long long>(a), vb = static_cast<long long>(b);             \
        if (va != vb) {                                                                       \
            std::printf("FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); \
            failures++;                                                                       \
        }                                                                                     \
    } while (0)

static Simulator run(const std::string& src, HazardMode mode, int penalty = 0, bool course_regs = false) {
    AsmResult r = assemble(src);
    if (!r.ok) std::printf("assemble error line %d: %s\n", r.error_line, r.error.c_str());
    CHECK(r.ok);
    Simulator sim;
    sim.cfg.hazard = mode;
    sim.cfg.miss_penalty = penalty;
    sim.cfg.course_regs = course_regs;
    sim.load(r.words);
    int guard = 0;
    while (sim.step() && ++guard < 100000) {}
    CHECK(sim.halted());
    return sim;
}

static void test_disassembler() {
    // Words from the course decoder project.
    CHECK(isa::disassemble(0x032BA020, 0x9A040) == "add $s4, $t9, $t3");
    CHECK(isa::disassemble(0x8CE90014, 0x9A044) == "lw $t1, 20($a3)");
    CHECK(isa::disassemble(0x12A90003, 0x9A048) == "beq $s5, $t1, 0x9A058");
    CHECK(isa::disassemble(0xAE8FFFF4, 0x9A058) == "sw $t7, -12($s4)");
    CHECK(isa::disassemble(0x158FFFF7, 0x9A064) == "bne $t4, $t7, 0x9A044");
    CHECK(isa::disassemble(0, 0) == "nop");
}

static void test_assembler_roundtrip() {
    const char* src =
        "start: add $t0, $t1, $t2\n sub $s0, $s1, $s2\n and $1, $2, $3\n or $4, $5, $6\n"
        " xor $t0, $t0, $t1\n nor $t0, $t0, $t1\n slt $v0, $a0, $a1\n sll $t0, $t1, 4\n srl $t0, $t1, 31\n"
        " addi $t0, $t0, -1\n slti $t0, $t1, 100\n andi $t0, $t1, 255\n ori $t0, $t1, 0xFFFF\n xori $t0, $t1, 1\n"
        " lui $t0, 0x1234\n lw $t0, 8($sp)\n sw $t0, -8($sp)\n lb $t0, ($t1)\n lbu $t0, 3($t1)\n sb $t0, 0($t1)\n"
        "mid: beq $t0, $t1, start\n bne $t0, $zero, end\n j mid\nend: nop\n";
    AsmResult r = assemble(src);
    CHECK(r.ok);
    CHECK_EQ(r.words.size(), 24);
    // Disassemble every word and assemble that text again: must give the same word.
    for (size_t i = 0; i < r.words.size(); i++) {
        uint32_t pc = static_cast<uint32_t>(i * 4);
        std::string text;
        for (size_t k = 0; k < i; k++) text += "nop\n";  // keep the address the same
        text += isa::disassemble(r.words[i], pc) + "\n";
        AsmResult again = assemble(text);
        CHECK(again.ok);
        if (again.ok) CHECK_EQ(again.words.back(), r.words[i]);
    }
    CHECK_EQ(r.words[0], 0x012A4020);   // add $t0,$t1,$t2
    CHECK_EQ(r.words[9], 0x2108FFFF);   // addi $t0,$t0,-1
    CHECK_EQ(r.words[20], 0x1109FFEB);  // beq back to 0 from 0x50: offset -21

    CHECK(!assemble("add $t0, $t1").ok);
    CHECK(!assemble("frob $t0").ok);
    CHECK(!assemble("beq $t0, $t1, nowhere").ok);
    CHECK(!assemble("addi $t0, $t0, 70000").ok);
    CHECK_EQ(assemble("nop\nbad $1").error_line, 2);

    AsmResult li = assemble("li $t0, 5\nli $t1, 0x12345678\nx: li $t2, 40000\n");
    CHECK(li.ok);
    CHECK_EQ(li.words.size(), 4);
    CHECK_EQ(li.labels.begin()->first, 12);
}

static void test_cache() {
    uint8_t mem[MEM_SIZE];
    for (int i = 0; i < MEM_SIZE; i++) mem[i] = static_cast<uint8_t>(i % 256);
    Cache c;
    uint8_t v = 0;
    CHECK(!c.read(0x040, mem, v));
    CHECK_EQ(v, 0x40);
    CHECK(c.read(0x04F, mem, v));
    CHECK_EQ(v, 0x4F);
    CHECK(c.write(0x045, 0xAB, mem));
    CHECK(c.line[4].dirty);
    CHECK_EQ(mem[0x045], 0x45);       // not written back yet
    bool wrote_back = false;
    CHECK(!c.read(0x140, mem, v, &wrote_back));  // same slot, different tag
    CHECK(wrote_back);
    CHECK_EQ(mem[0x045], 0xAB);
    CHECK_EQ(c.line[4].tag, 1);
    CHECK(!c.line[4].dirty);
    CHECK(!c.probe(0x045));
    CHECK(c.probe(0x14F));
}

static void test_pipeline() {
    const std::string sum = SAMPLES[0].source;
    Simulator fwd = run(sum, HazardMode::Forward);
    CHECK_EQ(fwd.state().regs[9], 55);
    CHECK_EQ(fwd.state().regs[8], 0);
    CHECK_EQ(fwd.state().regs[2], 55);
    CHECK_EQ(fwd.peek32(0x100), 55);
    CHECK_EQ(fwd.state().stats.retired, 2 + 30 + 2);
    CHECK_EQ(fwd.state().stats.taken, 9);
    CHECK_EQ(fwd.state().stats.branches, 10);
    CHECK_EQ(fwd.state().stats.flushed, 18);
    // 34 instructions + 4 to drain + 18 flushed + 1 load-use-free... sw then lw: no stall.
    CHECK_EQ(fwd.state().stats.cycles, 34 + 4 + 18);

    Simulator stall = run(sum, HazardMode::Stall);
    CHECK_EQ(stall.state().regs[9], 55);
    CHECK_EQ(stall.state().regs[2], 55);
    CHECK(stall.state().stats.cycles > fwd.state().stats.cycles);
    CHECK(stall.state().stats.data_stalls > 0);
    CHECK_EQ(stall.state().stats.forwards, 0);

    // Cache miss penalty changes timing but never results.
    Simulator slow = run(sum, HazardMode::Forward, 5);
    CHECK_EQ(slow.state().regs[2], 55);
    CHECK_EQ(slow.state().stats.cycles, fwd.state().stats.cycles + 5);  // one miss (sw), lw hits

    // Load-use: exactly one stall with forwarding, correct values.
    Simulator lu = run(SAMPLES[3].source, HazardMode::Forward);
    CHECK_EQ(lu.state().regs[9], 7);
    CHECK_EQ(lu.state().regs[10], 14);
    CHECK_EQ(lu.state().regs[11], 7);
    CHECK_EQ(lu.state().regs[12], 7);
    CHECK_EQ(lu.state().stats.data_stalls, 1);
    Simulator lu_stall = run(SAMPLES[3].source, HazardMode::Stall);
    CHECK_EQ(lu_stall.state().regs[10], 14);
    CHECK_EQ(lu_stall.state().regs[12], 7);
    // Without hazard handling the add reads the stale $t1 (0).
    Simulator lu_none = run(SAMPLES[3].source, HazardMode::None);
    CHECK_EQ(lu_none.state().regs[10], 0);

    // Fibonacci in memory.
    for (HazardMode m : {HazardMode::Forward, HazardMode::Stall}) {
        Simulator fib = run(SAMPLES[1].source, m, 2);
        const int expect[12] = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89};
        for (int i = 0; i < 12; i++) CHECK_EQ(fib.peek32(0x200 + 4 * i), expect[i]);
        CHECK_EQ(fib.state().regs[8], 144);
    }

    // Cache ping-pong: 3 misses in the first iteration, then 2 per iteration
    // (the sb leaves block A cached for the next lb), and the final lb hits.
    Simulator pp = run(SAMPLES[2].source, HazardMode::Forward);
    CHECK_EQ(pp.state().stats.misses, 9);
    CHECK_EQ(pp.state().stats.hits, 4);
    CHECK_EQ(pp.state().regs[12], static_cast<int8_t>(0x80));  // 0x40 + 0x40, sign-extended by lb

    // Jumps: skip one instruction, one squashed fetch.
    Simulator j = run("li $t0, 1\n j over\n li $t0, 2\nover: addi $t1, $t0, 10\n", HazardMode::Forward);
    CHECK_EQ(j.state().regs[8], 1);
    CHECK_EQ(j.state().regs[9], 11);
    CHECK_EQ(j.state().stats.flushed, 1);

    // Course project program runs to completion; sb/lb at 0x108.
    Simulator course = run(SAMPLES[4].source, HazardMode::Forward, 0, true);
    CHECK_EQ(course.peek8(0x108), 0x02);                   // sb $2 -> low byte of 0x102
    CHECK_EQ(course.state().regs[3], 0x104 + 0x103);       // add $3, $4, $3
    CHECK_EQ(course.state().stats.retired, 8);

    // Step back restores the exact previous state.
    AsmResult r = assemble(sum);
    Simulator sim;
    sim.load(r.words);
    for (int i = 0; i < 20; i++) sim.step();
    int32_t t1 = sim.state().regs[9];
    uint64_t cyc = sim.state().stats.cycles;
    for (int i = 0; i < 7; i++) sim.step();
    for (int i = 0; i < 7; i++) CHECK(sim.step_back());
    CHECK_EQ(sim.state().regs[9], t1);
    CHECK_EQ(sim.state().stats.cycles, cyc);
    CHECK_EQ(sim.history().size(), 20);
}

static void test_learning() {
    // Every lesson assembles, and every step's event is reached before the program ends.
    for (const Lesson& lesson : lessons()) {
        AsmResult r = assemble(lesson.source);
        CHECK(r.ok);
        Simulator sim;
        sim.cfg.hazard = static_cast<HazardMode>(lesson.hazard);
        sim.cfg.miss_penalty = lesson.penalty;
        sim.load(r.words);
        for (size_t i = 1; i < lesson.steps.size(); i++) {
            const LessonStep& st = lesson.steps[i];
            if (st.hazard >= 0) sim.cfg.hazard = static_cast<HazardMode>(st.hazard);
            if (st.restart) sim.reset();
            bool met = false;
            while (!met && sim.step()) met = until_met(st.until, sim.history().back());
            if (st.until == Until::Halt) CHECK(sim.halted());
            else if (!met) {
                std::printf("FAIL lesson '%s' step %d never reached its event\n", lesson.title, static_cast<int>(i));
                failures++;
            }
        }
        CHECK(sim.halted());
    }
    // Claims made in the lesson texts.
    const char* hazard = lessons()[1].source;
    Simulator none = run(hazard, HazardMode::None);
    CHECK_EQ(none.state().regs[9], 0);
    CHECK_EQ(none.state().regs[10], 0);
    Simulator fwd = run(hazard, HazardMode::Forward);
    CHECK_EQ(fwd.state().regs[9], 10);
    CHECK_EQ(fwd.state().regs[10], 15);
    CHECK_EQ(fwd.state().stats.data_stalls, 0);
    CHECK_EQ(run(lessons()[0].source, HazardMode::Forward).state().stats.cycles, 9);
    CHECK_EQ(run(lessons()[4].source, HazardMode::Forward).state().stats.flushed, 4);
    Simulator cache = run(lessons()[5].source, HazardMode::Forward, 3);
    CHECK_EQ(cache.state().stats.hits, 2);
    CHECK_EQ(cache.state().stats.misses, 2);

    // preview() must not disturb the simulator, and quiz answers must match what then happens.
    for (int sample = 0; sample < SAMPLE_COUNT; sample++) {
        AsmResult r = assemble(SAMPLES[sample].source);
        Simulator sim;
        sim.cfg.miss_penalty = sample == 2 ? 2 : 0;
        sim.load(r.words);
        int questions = 0;
        while (!sim.halted()) {
            const uint64_t cycles = sim.state().stats.cycles;
            const size_t hist = sim.history().size();
            Quiz q;
            const bool asked = make_quiz(sim, q);
            CHECK_EQ(sim.state().stats.cycles, cycles);
            CHECK_EQ(sim.history().size(), hist);
            sim.step();
            const Trace& t = sim.history().back();
            if (!asked) continue;
            questions++;
            CHECK(q.correct >= 0 && q.correct < static_cast<int>(q.options.size()));
            CHECK(!q.why.empty());
            if (q.options[0] == "Hit") CHECK_EQ(q.correct, (t.mem.happened && t.mem.hit) ? 0 : 1);
            if (q.options[0].rfind("Taken", 0) == 0) CHECK_EQ(q.correct, t.branch_taken ? 0 : 1);
            if (q.options.size() == 3 && q.options[1] == "Forwarded from EX/MEM")
                CHECK_EQ(q.correct, (t.fwd_a != Fwd::None ? t.fwd_a : t.fwd_b) == Fwd::ExMem ? 1 : 2);
            if (q.question.find("What value") != std::string::npos) CHECK(q.options[q.correct] == std::to_string(t.wb_value));
        }
        CHECK(questions > 0);
    }
    // Explanations exist for everything clickable.
    Simulator sim = run(SAMPLES[2].source, HazardMode::Forward);
    for (int i = 0; i < 5; i++) CHECK(!explain(sim, Selection{Selection::Stage, i}).what.empty());
    for (int i = 0; i < 4; i++) CHECK(!explain(sim, Selection{Selection::Latch, i}).now.empty());
    for (int i = 0; i < 32; i++) CHECK(!explain(sim, Selection{Selection::Reg, i}).now.empty());
    for (int i = 0; i < 16; i++) CHECK(!explain(sim, Selection{Selection::Slot, i}).now.empty());
}

int main() {
    test_learning();
    test_disassembler();
    test_assembler_roundtrip();
    test_cache();
    test_pipeline();
    if (failures == 0) std::printf("All core tests passed.\n");
    else std::printf("%d check(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
